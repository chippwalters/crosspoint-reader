// NoteEditorActivity implementation — see header for the model. Gated: empty TU without
// ENABLE_BLE_KEYBOARD.

#ifdef ENABLE_BLE_KEYBOARD

#include "NoteEditorActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <esp_system.h>

#include <algorithm>

#include "MappedInputManager.h"
#include "ble/BleKeyboardManager.h"
#include "ble/BleKeyboardStore.h"
#include "components/UITheme.h"
#include "fontIds.h"

// ---------------------------------------------------------------------------------------------
// Tunables. All #ifndef-overridable from build flags (same convention as the EPUB image guard)
// so the orchestrator can re-tune after hardware testing without touching code.
// ---------------------------------------------------------------------------------------------

// Runtime heap floor during a BLE session (REQUIRED guard). If internal free heap drops below
// this while the keyboard session is up, the note is flushed and the session is ended with a
// visible warning — fail loud, never OOM/crash. Gate context: peak free was 33.0 KB.
#ifndef NOTE_BLE_MIN_FREE_HEAP
#define NOTE_BLE_MIN_FREE_HEAP (25 * 1024)
#endif

// Auto-save: flush the tail to SD after this much typing silence (plus on every Enter and exit).
#ifndef NOTE_AUTOSAVE_IDLE_MS
#define NOTE_AUTOSAVE_IDLE_MS 1000
#endif

// How much of the end of the note is loaded into RAM on open (the editable/visible window).
#ifndef NOTE_TAIL_LOAD_BYTES
#define NOTE_TAIL_LOAD_BYTES 2048
#endif

// After a successful flush, if the tail grew past NOTE_TAIL_LOAD_BYTES it is trimmed from the
// front down to about this size (at a line boundary) to keep per-keystroke re-wrap bounded.
#ifndef NOTE_TAIL_TRIM_BYTES
#define NOTE_TAIL_TRIM_BYTES 1536
#endif

// GHOST_CLEAR cadence: run a clean full-frame refresh after this many NEW display lines
// (wraps + newlines) rendered with windowed fast refreshes.
#ifndef NOTE_GHOST_CLEAR_LINES
#define NOTE_GHOST_CLEAR_LINES 8
#endif

// Idle clean pass: when the user pauses typing this long and enough windowed refreshes have
// accumulated, run one clean full refresh to wipe A2 ghosting.
#ifndef NOTE_IDLE_CLEAN_MS
#define NOTE_IDLE_CLEAN_MS 3000
#endif
#ifndef NOTE_IDLE_CLEAN_MIN_WINDOWS
#define NOTE_IDLE_CLEAN_MIN_WINDOWS 10
#endif

// Heap-floor poll interval.
#ifndef NOTE_HEAP_CHECK_MS
#define NOTE_HEAP_CHECK_MS 2000
#endif

// Keep the device awake this long after the last BLE keystroke (see preventAutoSleep()).
#ifndef NOTE_KEY_AWAKE_MS
#define NOTE_KEY_AWAKE_MS 60000
#endif

// Refresh mode used for the ghost-clear / idle-clean full frames. HALF (~1.7 s) actually drives
// the ghost particles clean; FAST would just repaint over them.
static constexpr HalDisplay::RefreshMode NOTE_GHOST_CLEAR_REFRESH = HalDisplay::HALF_REFRESH;

// Editor body font. NotoSans 12 in normal builds; UI font in OMIT_FONTS builds.
#ifdef OMIT_FONTS
#define NOTE_FONT_ID UI_12_FONT_ID
#else
#define NOTE_FONT_ID NOTOSANS_12_FONT_ID
#endif

static constexpr size_t NOTE_KEY_QUEUE_LEN = 64;

// ---------------------------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------------------------

void NoteEditorActivity::onEnter() {
  Activity::onEnter();

  // HARD RULE: the C3 BLE controller deadlocks if inited below 80 MHz (10 MHz idle clock).
  // Hold normal CPU speed for the ENTIRE session — released in onExit after deinit.
  powerLock_ = std::make_unique<HalPowerManager::Lock>();

  {
    RenderLock lock(*this);  // SD + display share SPI
    fileLoadFailed_ = !loadTailFromDisk();
  }

  computeGeometry();
  rebuildLines();

  dirty_ = false;
  saveFailed_ = false;
  lastEditMs_ = millis();
  lastKeyMs_ = millis();  // grace period so the device doesn't sleep while the user starts typing
  lastHeapCheckMs_ = 0;
  linesSinceGhostClear_ = 0;
  windowRefreshesSinceClean_ = 0;
  reconnectAttempted_ = false;

  charQueue_ = xQueueCreate(NOTE_KEY_QUEUE_LEN, sizeof(char));
  if (!charQueue_) {
    LOG_ERR("NOTE", "key queue alloc failed — keyboard input disabled");
  }

  auto& kb = BleKeyboardManager::getInstance();
  if (charQueue_) {
    // Capture the queue handle BY VALUE (never `this`): the callback runs on the NimBLE host
    // task and must stay valid until deinit() kills the producer in onExit.
    QueueHandle_t q = charQueue_;
    kb.setCharCallback([q](char c) {
      if (xQueueSend(q, &c, 0) != pdTRUE) {
        LOG_ERR("NOTE", "key queue full — keystroke dropped");
      }
    });
  }

  if (!fileLoadFailed_ && charQueue_ && BLEKB_STORE.hasKeyboard()) {
    // Auto-reconnect to the bonded keyboard: direct connect by stored address (no scan), with a
    // scan fallback if the direct connect fails. Runs on a worker task so BACK stays responsive.
    kbSessionActive_ = true;
    kbUi_ = KbUi::CONNECTING;
    kb.startConnectAsync(BLEKB_STORE.getAddress(), BLEKB_STORE.getAddressType(), BLEKB_STORE.getDisplayName(),
                         /*scanFallback=*/true);
  } else {
    kbSessionActive_ = false;
    kbUi_ = KbUi::NONE_PAIRED;  // fail loud: header says "pair in Settings", never silent dead keys
  }

  fullRedrawPending_ = true;
  fullRedrawMode_ = HalDisplay::HALF_REFRESH;  // IDLE state: clean entry render
  requestUpdate();
}

void NoteEditorActivity::onExit() {
  Activity::onExit();
  // NOTE: onExit runs with the framework's RenderLock already held — do NOT take another.

  // COMMIT/EXIT: final flush. The BACK path in loop() already flushed (so it could fail loud
  // before leaving); this covers forced exits (sleep, crash-free teardown).
  if (dirty_) {
    if (!flushToDisk()) {
      LOG_ERR("NOTE", "final flush FAILED for %s — tail bytes may be lost", path_.c_str());
    }
  }

  // Tear the BLE session down, KEEPING bonds (production rule — the gate harness's deinit(true)
  // was harness-only). deinit joins the worker task, so the char callback's producer is dead
  // before the queue is deleted.
  auto& kb = BleKeyboardManager::getInstance();
  kb.deinit(false);
  kb.setCharCallback(nullptr);
  if (charQueue_) {
    vQueueDelete(charQueue_);
    charQueue_ = nullptr;
  }

  powerLock_.reset();  // last: normal power management resumes only after BLE is fully down
}

bool NoteEditorActivity::preventAutoSleep() {
  if (dirty_) return true;  // never sleep with unflushed text
  if (!kbSessionActive_) return false;
  auto& kb = BleKeyboardManager::getInstance();
  if (kb.isBusy()) return true;  // connect/scan in flight
  if (kbUi_ == KbUi::CONNECTED && millis() - lastKeyMs_ < NOTE_KEY_AWAKE_MS) return true;
  return false;  // idle long enough — let the normal sleep timeout reclaim the battery
}

// ---------------------------------------------------------------------------------------------
// File / text model
// ---------------------------------------------------------------------------------------------

bool NoteEditorActivity::loadTailFromDisk() {
  tail_.clear();
  fileBaseOffset_ = 0;

  if (!Storage.exists(path_.c_str())) {
    // Should have been created by the notes list; create it here so open-by-path always works.
    HalFile f = Storage.open(path_.c_str(), O_RDWR | O_CREAT);
    if (!f) {
      LOG_ERR("NOTE", "cannot create %s", path_.c_str());
      return false;
    }
    f.close();
    return true;
  }

  HalFile f = Storage.open(path_.c_str(), O_RDONLY);
  if (!f) {
    LOG_ERR("NOTE", "cannot open %s", path_.c_str());
    return false;
  }
  const size_t size = f.fileSize();
  const size_t loadLen = std::min(size, (size_t)NOTE_TAIL_LOAD_BYTES);
  fileBaseOffset_ = size - loadLen;
  if (loadLen > 0) {
    if (!f.seekSet(fileBaseOffset_)) {
      f.close();
      LOG_ERR("NOTE", "seek failed in %s", path_.c_str());
      return false;
    }
    tail_.resize(loadLen);
    const int n = f.read(&tail_[0], loadLen);
    if (n != (int)loadLen) {
      f.close();
      tail_.clear();
      LOG_ERR("NOTE", "short read in %s (%d/%u)", path_.c_str(), n, (unsigned)loadLen);
      return false;
    }
  }
  f.close();

  // If we landed mid-line (or mid-UTF-8 sequence), drop the partial first line so the display
  // starts clean. Skipped when the whole file fits — nothing to align then.
  if (fileBaseOffset_ > 0) {
    const size_t nl = tail_.find('\n');
    if (nl != std::string::npos && nl + 1 < tail_.size()) {
      fileBaseOffset_ += nl + 1;
      tail_.erase(0, nl + 1);
    } else if (nl == std::string::npos) {
      // One giant unbroken chunk: at least resynchronize to a UTF-8 boundary.
      size_t drop = 0;
      while (drop < tail_.size() && (static_cast<unsigned char>(tail_[drop]) & 0xC0) == 0x80) ++drop;
      fileBaseOffset_ += drop;
      tail_.erase(0, drop);
    }
  }
  LOG_DBG("NOTE", "loaded %s: size=%u base=%u tail=%u", path_.c_str(), (unsigned)size,
          (unsigned)fileBaseOffset_, (unsigned)tail_.size());
  return true;
}

bool NoteEditorActivity::extendTailBack() {
  if (fileBaseOffset_ == 0) return false;
  const size_t chunk = std::min(fileBaseOffset_, (size_t)1024);
  const size_t newBase = fileBaseOffset_ - chunk;

  HalFile f = Storage.open(path_.c_str(), O_RDONLY);
  if (!f) {
    LOG_ERR("NOTE", "extend: cannot open %s", path_.c_str());
    return false;
  }
  std::string prefix;
  prefix.resize(chunk);
  bool ok = f.seekSet(newBase) && f.read(&prefix[0], chunk) == (int)chunk;
  f.close();
  if (!ok) {
    LOG_ERR("NOTE", "extend: read failed in %s", path_.c_str());
    return false;
  }
  // Disk content below the old base is by definition unchanged (edits only ever touch the end),
  // so prepending is always consistent — even with unflushed edits in tail_.
  tail_.insert(0, prefix);
  fileBaseOffset_ = newBase;
  LOG_DBG("NOTE", "extended tail back to base=%u (tail=%u)", (unsigned)fileBaseOffset_, (unsigned)tail_.size());
  return true;
}

bool NoteEditorActivity::flushToDisk() {
  // Bounded tail rewrite: the file below fileBaseOffset_ is never touched. Writes ≤ tail_.size()
  // (≤ ~2 KB in steady state) regardless of note length; truncate persists backspace shrinks.
  HalFile f = Storage.open(path_.c_str(), O_RDWR | O_CREAT);
  if (!f) {
    LOG_ERR("NOTE", "flush: cannot open %s", path_.c_str());
    saveFailed_ = true;
    return false;
  }
  bool ok = f.seekSet(fileBaseOffset_);
  if (ok && !tail_.empty()) {
    ok = f.write(tail_.data(), tail_.size()) == tail_.size();
  }
  if (ok) {
    ok = f.truncate(fileBaseOffset_ + tail_.size());
  }
  f.flush();
  f.close();
  if (!ok) {
    LOG_ERR("NOTE", "flush FAILED for %s", path_.c_str());
    saveFailed_ = true;
    return false;
  }
  dirty_ = false;
  if (saveFailed_) {
    saveFailed_ = false;  // recovered — clear the banner on the next repaint
    statusRedrawPending_ = true;
  }
  trimTailAfterFlush();
  return true;
}

void NoteEditorActivity::trimTailAfterFlush() {
  if (tail_.size() <= (size_t)NOTE_TAIL_LOAD_BYTES) return;
  // Trim from the front to ~NOTE_TAIL_TRIM_BYTES, preferring a line boundary. The trimmed bytes
  // are already flushed; the visible slice (last lines) is unaffected, so no redraw is needed.
  size_t cut = tail_.size() - (size_t)NOTE_TAIL_TRIM_BYTES;
  const size_t nl = tail_.find('\n', cut);
  if (nl != std::string::npos && nl + 1 < tail_.size()) {
    cut = nl + 1;
  } else {
    while (cut < tail_.size() && (static_cast<unsigned char>(tail_[cut]) & 0xC0) == 0x80) ++cut;
  }
  fileBaseOffset_ += cut;
  tail_.erase(0, cut);
  rebuildLines();
}

void NoteEditorActivity::removeLastChar() {
  if (tail_.empty()) return;
  // UTF-8 safe: drop continuation bytes, then the lead byte.
  size_t n = tail_.size();
  while (n > 0 && (static_cast<unsigned char>(tail_[n - 1]) & 0xC0) == 0x80) --n;
  if (n > 0) --n;
  tail_.resize(n);
}

// ---------------------------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------------------------

void NoteEditorActivity::computeGeometry() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  fontId_ = NOTE_FONT_ID;
  lineHeight_ = renderer.getLineHeight(fontId_);
  if (lineHeight_ <= 0) lineHeight_ = 24;  // font lookup failure fail-safe: never divide by zero

  statusY_ = metrics.topPadding + metrics.headerHeight + 2;
  const int statusH = renderer.getLineHeight(SMALL_FONT_ID) + 4;

  textX_ = metrics.contentSidePadding;
  textY_ = statusY_ + statusH + metrics.verticalSpacing;
  // Reserve a little slack for the cursor bar and token-boundary kerning (wrap measures tokens
  // independently, so a couple of px of error is possible).
  textW_ = pageWidth - 2 * metrics.contentSidePadding - 8;
  const int textH = pageHeight - textY_ - metrics.buttonHintsHeight - metrics.verticalSpacing;
  visibleLines_ = std::max(1, textH / lineHeight_);
}

// Greedy word-wrap of one paragraph. Widths are accumulated incrementally (token = word +
// trailing spaces measured once each), so a full rebuild is O(bytes) in measurement calls — the
// tail is capped at ~2 KB, making per-keystroke rebuilds cheap next to the ~150-250 ms panel
// refresh. Trailing spaces ride along invisibly at line ends (typewriter convention).
static void wrapParagraphInto(const GfxRenderer& renderer, int fontId, int wrapW, const std::string& para,
                              std::vector<std::string>& out) {
  if (para.empty()) {
    out.emplace_back();
    return;
  }
  std::string cur;
  int curW = 0;
  const int spaceW = renderer.getSpaceWidth(fontId);
  size_t i = 0;
  const size_t n = para.size();
  while (i < n) {
    const size_t wordStart = i;
    while (i < n && para[i] != ' ') ++i;
    std::string word = para.substr(wordStart, i - wordStart);
    size_t spaceCount = 0;
    while (i < n && para[i] == ' ') {
      ++spaceCount;
      ++i;
    }
    int wordW = word.empty() ? 0 : renderer.getTextAdvanceX(fontId, word.c_str(), EpdFontFamily::REGULAR);

    if (!cur.empty() && curW + wordW > wrapW) {
      out.push_back(cur);
      cur.clear();
      curW = 0;
    }
    // Hard-split a word wider than the whole line (unbroken strings, URLs...): peel off the
    // largest prefix that fits, one UTF-8 char at a time (always ≥1 char for progress).
    while (wordW > wrapW && !word.empty()) {
      size_t split = 0;
      int w = 0;
      while (split < word.size()) {
        size_t next = split + 1;
        while (next < word.size() && (static_cast<unsigned char>(word[next]) & 0xC0) == 0x80) ++next;
        const std::string ch = word.substr(split, next - split);
        const int chW = renderer.getTextAdvanceX(fontId, ch.c_str(), EpdFontFamily::REGULAR);
        if (split > 0 && w + chW > wrapW) break;
        w += chW;
        split = next;
      }
      out.push_back(word.substr(0, split));
      word.erase(0, split);
      wordW = word.empty() ? 0 : renderer.getTextAdvanceX(fontId, word.c_str(), EpdFontFamily::REGULAR);
    }
    cur += word;
    curW += wordW;
    cur.append(spaceCount, ' ');
    curW += static_cast<int>(spaceCount) * spaceW;
  }
  out.push_back(cur);
}

void NoteEditorActivity::rebuildLines() {
  lines_.clear();
  size_t pos = 0;
  while (true) {
    const size_t nl = tail_.find('\n', pos);
    const std::string para = tail_.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
    wrapParagraphInto(renderer, fontId_, textW_, para, lines_);
    if (nl == std::string::npos) break;
    pos = nl + 1;
  }
  if (lines_.empty()) lines_.emplace_back();
}

std::vector<std::string> NoteEditorActivity::visibleSlice() const {
  const int total = static_cast<int>(lines_.size());
  const int start = std::max(0, total - visibleLines_);
  return std::vector<std::string>(lines_.begin() + start, lines_.end());
}

// ---------------------------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------------------------

void NoteEditorActivity::handleChar(char c, bool& structuralChange, bool& newlineSeen) {
  if (c == 0x08) {  // Backspace
    if (tail_.empty() && !extendTailBack()) {
      return;  // true start of file — nothing to delete
    }
    if (!tail_.empty()) {
      removeLastChar();
      dirty_ = true;
      structuralChange = true;  // may unwrap/join lines — recompute
    }
    return;
  }
  if (c == '\n') {
    tail_ += '\n';
    dirty_ = true;
    structuralChange = true;
    newlineSeen = true;
    return;
  }
  if (c == '\t') {
    tail_ += "  ";  // fonts have no tab advance; two spaces is the typewriter-friendly stand-in
    dirty_ = true;
    return;
  }
  if (c >= 0x20 && c < 0x7F) {  // printable ASCII (HID decoder emits nothing else)
    tail_ += c;
    dirty_ = true;
  }
}

// ---------------------------------------------------------------------------------------------
// Main loop
// ---------------------------------------------------------------------------------------------

void NoteEditorActivity::loop() {
  auto& kb = BleKeyboardManager::getInstance();

  // --- BACK = final flush + exit (fail loud if the save failed) ---
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    bool flushOk = true;
    if (dirty_) {
      RenderLock lock(*this);
      flushOk = flushToDisk();
    }
    if (!flushOk) {
      // Never leave silently with lost text: a full-screen error the user must acknowledge.
      activityManager.goToFullScreenMessage("Note save FAILED - check SD card", EpdFontFamily::BOLD);
    } else {
      finish();
    }
    return;
  }

  // --- Keyboard session state machine (async worker owned by BleKeyboardManager) ---
  if (kbSessionActive_) {
    const bool busy = kb.isBusy();
    KbUi newUi = kbUi_;
    if (!busy) {
      switch (kb.getState()) {
        case BleKeyboardManager::State::CONNECTED:
          newUi = KbUi::CONNECTED;
          reconnectAttempted_ = false;
          break;
        case BleKeyboardManager::State::DISCONNECTED:
          if (!reconnectAttempted_ && BLEKB_STORE.hasKeyboard()) {
            // One automatic reconnect (keyboard woke from its own sleep, brief range drop...).
            reconnectAttempted_ = true;
            newUi = KbUi::RECONNECTING;
            kb.startConnectAsync(BLEKB_STORE.getAddress(), BLEKB_STORE.getAddressType(),
                                 BLEKB_STORE.getDisplayName(), /*scanFallback=*/false);
          } else {
            newUi = KbUi::DISCONNECTED;
          }
          break;
        case BleKeyboardManager::State::FAILED:
          newUi = (kbUi_ == KbUi::RECONNECTING || reconnectAttempted_) ? KbUi::DISCONNECTED : KbUi::FAILED;
          break;
        default:
          break;
      }
    }
    if (newUi != kbUi_) {
      kbUi_ = newUi;
      statusRedrawPending_ = true;
      requestUpdate();
    }

    // --- Runtime heap-floor guard (REQUIRED): never let a BLE session OOM the firmware ---
    if (millis() - lastHeapCheckMs_ >= NOTE_HEAP_CHECK_MS) {
      lastHeapCheckMs_ = millis();
      const uint32_t freeHeap = esp_get_free_heap_size();
      if (freeHeap < NOTE_BLE_MIN_FREE_HEAP) {
        LOG_ERR("NOTE", "heap floor tripped (%u < %u): saving + ending BLE session", (unsigned)freeHeap,
                (unsigned)NOTE_BLE_MIN_FREE_HEAP);
        {
          RenderLock lock(*this);
          flushToDisk();
        }
        kb.deinit(false);  // graceful: bonds kept, controller heap released
        kbSessionActive_ = false;
        kbUi_ = KbUi::LOW_MEM;
        fullRedrawPending_ = true;  // full repaint so the warning is unmissable
        fullRedrawMode_ = HalDisplay::FAST_REFRESH;
        requestUpdate();
        return;
      }
    }
  }

  // --- Drain decoded keystrokes → mutate the text model → schedule the A2 render ---
  if (charQueue_ != nullptr && uxQueueMessagesWaiting(charQueue_) > 0 && !fileLoadFailed_) {
    RenderLock lock(*this);  // render() reads lines_; mutate under the lock

    const std::vector<std::string> oldVisible = visibleSlice();
    const int oldLineCount = static_cast<int>(lines_.size());

    bool structuralChange = false;
    bool newlineSeen = false;
    char c;
    while (xQueueReceive(charQueue_, &c, 0) == pdTRUE) {
      handleChar(c, structuralChange, newlineSeen);
    }
    lastKeyMs_ = millis();
    lastEditMs_ = millis();

    rebuildLines();
    (void)structuralChange;  // rebuildLines() recomputes everything; kept for readability above

    // Diff the visible slices to decide between a windowed line refresh and a full frame.
    const std::vector<std::string> newVisible = visibleSlice();
    const int newLineCount = static_cast<int>(lines_.size());
    if (newLineCount > oldLineCount) {
      linesSinceGhostClear_ += newLineCount - oldLineCount;
    }

    const size_t maxRows = std::max(oldVisible.size(), newVisible.size());
    int firstDiff = -1;
    int lastDiff = -1;
    for (size_t i = 0; i < maxRows; ++i) {
      const std::string* a = i < oldVisible.size() ? &oldVisible[i] : nullptr;
      const std::string* b = i < newVisible.size() ? &newVisible[i] : nullptr;
      const bool differs = (a == nullptr) != (b == nullptr) || (a && b && *a != *b);
      if (differs) {
        if (firstDiff < 0) firstDiff = static_cast<int>(i);
        lastDiff = static_cast<int>(i);
      }
    }

    if (firstDiff >= 0) {
      const bool ghostClearDue = linesSinceGhostClear_ >= NOTE_GHOST_CLEAR_LINES;
      const bool scrolled = oldVisible.size() == newVisible.size() && firstDiff == 0 &&
                            static_cast<int>(newVisible.size()) >= visibleLines_;
      // Heuristic: >2 changed rows means a scroll/reflow — a windowed refresh would be as big as
      // the frame anyway. Ghost-clear promotes to a clean HALF frame.
      if (ghostClearDue || scrolled || (lastDiff - firstDiff) > 2) {
        fullRedrawPending_ = true;
        fullRedrawMode_ = ghostClearDue ? NOTE_GHOST_CLEAR_REFRESH : HalDisplay::FAST_REFRESH;
        if (ghostClearDue) linesSinceGhostClear_ = 0;
      } else if (!fullRedrawPending_) {
        // TYPING: window over just the changed line span (usually the one input line).
        if (windowRedrawPending_) {
          windowFromVisible_ = std::min(windowFromVisible_, firstDiff);
          windowToVisible_ = std::max(windowToVisible_, lastDiff);
        } else {
          windowRedrawPending_ = true;
          windowFromVisible_ = firstDiff;
          windowToVisible_ = lastDiff;
        }
      }
      requestUpdate();
    }

    // Auto-save on every newline (typing pause flush below covers the rest).
    if (newlineSeen && dirty_) {
      flushToDisk();  // still under RenderLock
    }
    return;  // render this batch before doing anything else
  }

  // --- Auto-save on typing pause ---
  if (dirty_ && millis() - lastEditMs_ >= NOTE_AUTOSAVE_IDLE_MS) {
    RenderLock lock(*this);
    flushToDisk();
    if (saveFailed_) {
      statusRedrawPending_ = true;  // fail loud in the header
      requestUpdate();
    }
  }

  // --- Idle GHOST_CLEAR: wipe accumulated A2 ghosting once the user pauses ---
  if (!dirty_ && !fullRedrawPending_ && !windowRedrawPending_ &&
      windowRefreshesSinceClean_ >= NOTE_IDLE_CLEAN_MIN_WINDOWS && millis() - lastKeyMs_ >= NOTE_IDLE_CLEAN_MS) {
    fullRedrawPending_ = true;
    fullRedrawMode_ = NOTE_GHOST_CLEAR_REFRESH;
    linesSinceGhostClear_ = 0;
    requestUpdate();
  }
}

// ---------------------------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------------------------

std::string NoteEditorActivity::statusLineText() const {
  std::string s;
  if (fileLoadFailed_) return "FILE ERROR - cannot edit this note";
  if (saveFailed_) s += "SAVE FAILED! ";
  auto& kb = BleKeyboardManager::getInstance();
  switch (kbUi_) {
    case KbUi::NONE_PAIRED:
      s += "No keyboard - pair in Settings > Keyboard";
      break;
    case KbUi::CONNECTING:
      s += "Keyboard: connecting...";
      break;
    case KbUi::CONNECTED: {
      std::string name = kb.getConnectedName();
      if (name.empty()) name = "keyboard";
      s += "Keyboard: " + name + " connected";
      break;
    }
    case KbUi::RECONNECTING:
      s += "Keyboard: reconnecting...";
      break;
    case KbUi::DISCONNECTED:
      s += "Keyboard DISCONNECTED - keys inactive";
      break;
    case KbUi::FAILED:
      s += "Keyboard connect FAILED - check it is on";
      break;
    case KbUi::LOW_MEM:
      s += "LOW MEMORY: note saved, keyboard session ended";
      break;
  }
  return s;
}

void NoteEditorActivity::drawLineSpan(const int firstVisible, const int lastVisible) const {
  const std::vector<std::string> vis = visibleSlice();
  const int last = std::min(lastVisible, visibleLines_ - 1);
  const int spanY = textY_ + firstVisible * lineHeight_;
  const int spanH = (last - firstVisible + 1) * lineHeight_;

  renderer.fillRect(textX_ - 2, spanY, textW_ + 8, spanH, false);  // white out the span
  for (int i = firstVisible; i <= last && i < static_cast<int>(vis.size()); ++i) {
    if (!vis[i].empty()) {
      renderer.drawText(fontId_, textX_, textY_ + i * lineHeight_, vis[i].c_str());
    }
  }
  // Cursor: 2 px bar after the last character of the input line.
  const int cursorLine = static_cast<int>(vis.size()) - 1;
  if (cursorLine >= firstVisible && cursorLine <= last) {
    int cx = textX_;
    if (!vis[cursorLine].empty()) {
      cx += renderer.getTextAdvanceX(fontId_, vis[cursorLine].c_str(), EpdFontFamily::REGULAR);
    }
    cx = std::min(cx, textX_ + textW_ + 2);
    renderer.fillRect(cx + 1, textY_ + cursorLine * lineHeight_ + 2, 2, lineHeight_ - 4, true);
  }
}

void NoteEditorActivity::drawFullScreen() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, title_.c_str());

  const std::string status =
      renderer.truncatedText(SMALL_FONT_ID, statusLineText().c_str(), pageWidth - 2 * metrics.contentSidePadding);
  renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding, statusY_, status.c_str());

  const std::vector<std::string> vis = visibleSlice();
  drawLineSpan(0, static_cast<int>(vis.size()) - 1);

  // "Exit" (not "Save & exit"): auto-save is continuous, so there is never unsaved state to warn
  // about, and the longer label overflowed the 80px hint button. Same label as the other
  // session-style activities (CalibreConnect, web server).
  const auto labels = mappedInput.mapLabels(tr(STR_EXIT), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void NoteEditorActivity::render(RenderLock&&) {
  // Catch-all: framework-triggered renders (e.g. USB battery change) with nothing scheduled
  // repaint the whole frame.
  if (fullRedrawPending_ || (!windowRedrawPending_ && !statusRedrawPending_)) {
    drawFullScreen();
    renderer.displayBuffer(fullRedrawMode_);
    fullRedrawPending_ = false;
    windowRedrawPending_ = false;
    statusRedrawPending_ = false;
    windowRefreshesSinceClean_ = 0;
    fullRedrawMode_ = HalDisplay::FAST_REFRESH;  // next unscheduled repaint defaults to FAST
    return;
  }

  if (statusRedrawPending_) {
    statusRedrawPending_ = false;
    const auto& metrics = UITheme::getInstance().getMetrics();
    const int pageWidth = renderer.getScreenWidth();
    const int statusH = renderer.getLineHeight(SMALL_FONT_ID) + 4;
    renderer.fillRect(0, statusY_, pageWidth, statusH, false);
    const std::string status =
        renderer.truncatedText(SMALL_FONT_ID, statusLineText().c_str(), pageWidth - 2 * metrics.contentSidePadding);
    renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding, statusY_, status.c_str());
    renderer.displayWindow(0, statusY_, pageWidth, statusH);
  }

  if (windowRedrawPending_) {
    windowRedrawPending_ = false;
    const int first = std::max(0, windowFromVisible_);
    const int last = std::min(windowToVisible_, visibleLines_ - 1);
    if (last >= first) {
      drawLineSpan(first, last);
      renderer.displayWindow(textX_ - 2, textY_ + first * lineHeight_, textW_ + 8,
                             (last - first + 1) * lineHeight_);
      ++windowRefreshesSinceClean_;
    }
  }
}

#endif  // ENABLE_BLE_KEYBOARD
