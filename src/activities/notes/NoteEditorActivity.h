// NoteEditorActivity — the PaperBit "typewriter" note editor (BLE keyboard text entry).
//
// v1 scope (approved design): APPEND + BACKSPACE + ENTER only. No cursor movement, no selection.
// Typing appends at the end of the note; Backspace deletes the last character (works back across
// newlines and back past the loaded window — the tail is extended from disk on demand); Enter
// inserts a newline. Plain text: Markdown characters are just characters.
//
// Text model ("tail buffer"): the file on SD is the source of truth. Only the LAST ~2 KB of the
// file is held in RAM (tail_), starting at disk offset fileBaseOffset_. All edits happen at the
// end of tail_, so a flush is a bounded write: seek(fileBaseOffset_) + write(tail_) +
// truncate(fileBaseOffset_ + tail_.size()). Backspace edits therefore persist correctly (the
// truncate shrinks the file). Auto-save: on a ~1 s typing pause, on every Enter, and on exit.
//
// Rendering (plan §4 A2 state machine): IDLE = clean HALF render on entry; TYPING = re-render
// only the changed line span of the framebuffer and push it with a windowed FAST refresh
// (GfxRenderer::displayWindow → EInkDisplay::displayWindow, wired for this feature);
// GHOST_CLEAR = full HALF refresh every NOTE_GHOST_CLEAR_LINES new display lines (and as an
// idle clean pass); scroll = full-frame FAST; COMMIT/EXIT = final flush.
//
// Radio + lifecycle rules: BLE lives ONLY inside this activity (and the pairing screen).
// onEnter: HalPowerManager::Lock held for the whole session (the C3 BLE controller deadlocks
// below 80 MHz — hardware-found rule), Wi-Fi stopped, async connect to the bonded keyboard.
// onExit: final flush → BleKeyboardManager::deinit(false) (bonds KEPT) → lock released.
//
// Only compiled under ENABLE_BLE_KEYBOARD (empty TU otherwise).
#pragma once

#ifdef ENABLE_BLE_KEYBOARD

#include <HalDisplay.h>
#include <HalPowerManager.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include <memory>
#include <string>
#include <vector>

#include "activities/Activity.h"

class NoteEditorActivity final : public Activity {
 public:
  explicit NoteEditorActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string notePath,
                              std::string noteTitle)
      : Activity("NoteEditor", renderer, mappedInput),
        path_(std::move(notePath)),
        title_(std::move(noteTitle)) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  // Keep the device awake while a keyboard session is live (BLE keystrokes don't touch the GPIO
  // activity timer). Bounded: goes false ~60 s after the last keystroke so a forgotten open
  // editor still auto-sleeps eventually (battery first).
  bool preventAutoSleep() override;

 private:
  // Keyboard-session status shown in the header status line (fail loud, never silent dead keys).
  enum class KbUi : uint8_t {
    NONE_PAIRED,   // no bonded keyboard — tell the user to pair in Settings
    CONNECTING,    // async connect in flight (1-3 s typical)
    CONNECTED,     // subscribed; keystrokes flowing
    RECONNECTING,  // link dropped; one automatic reconnect attempt in flight
    DISCONNECTED,  // link dropped and reconnect failed/exhausted
    FAILED,        // connect failed outright
    LOW_MEM        // runtime heap floor tripped: session ended, note saved
  };

  // --- File / text model ---
  bool loadTailFromDisk();       // open (create if missing), read last NOTE_TAIL_LOAD_BYTES
  bool extendTailBack();         // backspace hit the start of the loaded tail: pull more from disk
  bool flushToDisk();            // bounded tail rewrite: seek(base) + write(tail) + truncate
  void trimTailAfterFlush();     // keep tail_ bounded; advances fileBaseOffset_
  void removeLastChar();         // UTF-8-safe backspace on tail_

  // --- Layout ---
  void computeGeometry();
  void rebuildLines();  // full greedy word-wrap of tail_ into lines_ (O(tail), see .cpp)
  std::vector<std::string> visibleSlice() const;

  // --- Input ---
  void handleChar(char c, bool& structuralChange, bool& newlineSeen);

  // --- Render helpers (called from render(), under RenderLock) ---
  void drawFullScreen() const;
  void drawLineSpan(int firstVisible, int lastVisible) const;
  std::string statusLineText() const;

  std::string path_;
  std::string title_;

  // Tail buffer model
  std::string tail_;
  size_t fileBaseOffset_ = 0;  // disk offset where tail_[0] lives
  bool dirty_ = false;
  bool saveFailed_ = false;   // latched until a flush succeeds; surfaced in the status line
  bool fileLoadFailed_ = false;
  unsigned long lastEditMs_ = 0;
  unsigned long lastKeyMs_ = 0;
  unsigned long lastHeapCheckMs_ = 0;

  // Wrapped display lines of tail_ (derived state; rebuilt on mutation, read by render()).
  std::vector<std::string> lines_;

  // Geometry (computed once in onEnter)
  int fontId_ = 0;
  int lineHeight_ = 0;
  int textX_ = 0;
  int textY_ = 0;
  int textW_ = 0;
  int visibleLines_ = 0;
  int statusY_ = 0;

  // Render coordination: loop() mutates under a RenderLock scope, render() consumes.
  bool fullRedrawPending_ = true;
  HalDisplay::RefreshMode fullRedrawMode_ = HalDisplay::HALF_REFRESH;
  bool windowRedrawPending_ = false;
  bool statusRedrawPending_ = false;
  int windowFromVisible_ = 0;
  int windowToVisible_ = 0;

  // A2 ghost management
  int linesSinceGhostClear_ = 0;
  int windowRefreshesSinceClean_ = 0;

  // Keyboard session
  QueueHandle_t charQueue_ = nullptr;
  std::unique_ptr<HalPowerManager::Lock> powerLock_;
  KbUi kbUi_ = KbUi::NONE_PAIRED;
  bool kbSessionActive_ = false;   // BLE stack up (until LOW_MEM teardown or exit)
  bool reconnectAttempted_ = false;
};

#endif  // ENABLE_BLE_KEYBOARD
