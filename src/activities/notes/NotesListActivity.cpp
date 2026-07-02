// NotesListActivity implementation — see header. Gated: empty TU without ENABLE_BLE_KEYBOARD.

#ifdef ENABLE_BLE_KEYBOARD

#include "NotesListActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "NoteEditorActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/BookCacheUtils.h"

namespace {
constexpr const char* kNotesDir = "/Notes";
constexpr unsigned long kDeleteHoldMs = 1000;  // same hold-Confirm gesture as FileBrowserActivity
constexpr size_t kMaxNotesListed = 100;
constexpr size_t kTitleScanBytes = 512;  // how far into the file we look for a "# " heading
// Wall-clock sanity floor for timestamp filenames: 2020-01-01 UTC. The X4 has no RTC (and the
// X3's DS3231 only tracks hour/minute), so system time is only valid after an NTP sync this
// boot — otherwise we fall back to sequential names instead of faking "1970-01-01" dates.
constexpr time_t kMinValidEpoch = 1577836800;

bool hasMdExtension(const char* name) {
  const size_t len = strlen(name);
  return len > 3 && strcasecmp(name + len - 3, ".md") == 0;
}
}  // namespace

void NotesListActivity::onEnter() {
  Activity::onEnter();
  // Entered from Home with Confirm still held: swallow that release so we don't instantly
  // trigger "+ New note" (same guard as FileBrowserActivity).
  lockNextConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);
  selectorIndex = 0;
  {
    RenderLock lock(*this);  // SD + display share SPI
    loadNotes();
  }
  requestUpdate();
}

void NotesListActivity::onExit() {
  Activity::onExit();
  notes.clear();
}

void NotesListActivity::loadNotes() {
  notes.clear();
  loadFailed = false;

  if (!Storage.ensureDirectoryExists(kNotesDir)) {
    LOG_ERR("NOTES", "cannot create %s", kNotesDir);
    loadFailed = true;  // fail loud: rendered as an explicit error line, not an empty list
    return;
  }

  auto root = Storage.open(kNotesDir);
  if (!root || !root.isDirectory()) {
    LOG_ERR("NOTES", "cannot open %s", kNotesDir);
    loadFailed = true;
    return;
  }
  root.rewindDirectory();

  char nameBuf[256];
  size_t skipped = 0;
  for (auto file = root.openNextFile(); file; file = root.openNextFile()) {
    file.getName(nameBuf, sizeof(nameBuf));
    if (file.isDirectory() || nameBuf[0] == '.' || !hasMdExtension(nameBuf)) continue;
    if (notes.size() >= kMaxNotesListed) {
      ++skipped;
      continue;
    }
    NoteEntry e;
    e.fileName = nameBuf;
    notes.push_back(std::move(e));
  }
  root.close();
  if (skipped > 0) {
    LOG_ERR("NOTES", "%u notes beyond the %u-entry cap are not listed", (unsigned)skipped,
            (unsigned)kMaxNotesListed);
  }

  // Newest-first. Notes created here have zero-padded timestamp (or sequence) names, so a
  // descending name sort IS newest-first for them; foreign .md files dropped into /Notes sort
  // deterministically by name among them (HalFile exposes no mtime).
  std::sort(notes.begin(), notes.end(),
            [](const NoteEntry& a, const NoteEntry& b) { return a.fileName > b.fileName; });

  // Display titles (first "# " heading, else filename stem).
  for (auto& e : notes) {
    e.title = extractTitle(std::string(kNotesDir) + "/" + e.fileName, e.fileName);
  }
}

std::string NotesListActivity::extractTitle(const std::string& fullPath, const std::string& fileName) {
  char buf[kTitleScanBytes + 1];
  const size_t n = Storage.readFileToBuffer(fullPath.c_str(), buf, sizeof(buf), kTitleScanBytes);
  buf[n] = '\0';

  // Find the first line that starts with "# " (heading level 1).
  const char* p = buf;
  while (p && *p) {
    if (p[0] == '#' && p[1] == ' ') {
      const char* start = p + 2;
      const char* end = strchr(start, '\n');
      std::string title = end ? std::string(start, end - start) : std::string(start);
      while (!title.empty() && (title.back() == '\r' || title.back() == ' ')) title.pop_back();
      if (!title.empty()) return title;
    }
    const char* nl = strchr(p, '\n');
    p = nl ? nl + 1 : nullptr;
  }

  // No heading: filename without the .md extension.
  return fileName.substr(0, fileName.size() - 3);
}

std::string NotesListActivity::makeNewNoteFileName() const {
  // Preferred: auto-timestamp "YYYY-MM-DD HHMM.md" in local time (status-bar UTC offset applied).
  // Only when the system wall clock is credible — see kMinValidEpoch.
  const time_t now = time(nullptr);
  if (now >= kMinValidEpoch) {
    uint8_t offQ = SETTINGS.clockUtcOffsetQ;
    if (offQ > 104) offQ = 104;  // clamp corrupted persisted values (same rule as HalClock)
    const time_t local = now + (static_cast<int>(offQ) - 48) * 15 * 60;
    struct tm tmv;
    gmtime_r(&local, &tmv);
    char name[32];
    snprintf(name, sizeof(name), "%04d-%02d-%02d %02d%02d.md", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
             tmv.tm_hour, tmv.tm_min);
    return name;
  }

  // Fallback (no valid clock — normal on the X4, which has no RTC): sequential "Note NNNN.md".
  // Zero-padded so the descending name sort keeps newest-first. Never fake a date.
  unsigned maxSeq = 0;
  for (const auto& e : notes) {
    unsigned v = 0;
    if (sscanf(e.fileName.c_str(), "Note %u.md", &v) == 1 && v > maxSeq) maxSeq = v;
  }
  char name[24];
  snprintf(name, sizeof(name), "Note %04u.md", maxSeq + 1);
  return name;
}

void NotesListActivity::createNewNote() {
  RenderLock lock(*this);
  std::string fileName = makeNewNoteFileName();
  std::string fullPath = std::string(kNotesDir) + "/" + fileName;

  // Collision (two notes in the same minute): append a counter rather than clobbering.
  int suffix = 2;
  while (Storage.exists(fullPath.c_str()) && suffix < 100) {
    const std::string stem = fileName.substr(0, fileName.size() - 3);
    char buf[64];
    snprintf(buf, sizeof(buf), "%s (%d).md", stem.substr(0, stem.find(" (")).c_str(), suffix++);
    fileName = buf;
    fullPath = std::string(kNotesDir) + "/" + fileName;
  }

  HalFile f = Storage.open(fullPath.c_str(), O_RDWR | O_CREAT);
  if (!f) {
    LOG_ERR("NOTES", "cannot create %s", fullPath.c_str());
    lock.unlock();
    activityManager.goToFullScreenMessage("Cannot create note - check SD card", EpdFontFamily::BOLD);
    return;
  }
  f.close();
  LOG_INF("NOTES", "created %s", fullPath.c_str());
  lock.unlock();

  openEditor(fileName, fileName.substr(0, fileName.size() - 3));
}

void NotesListActivity::openEditor(const std::string& fileName, const std::string& title) {
  const std::string fullPath = std::string(kNotesDir) + "/" + fileName;
  startActivityForResult(std::make_unique<NoteEditorActivity>(renderer, mappedInput, fullPath, title),
                         [this](const ActivityResult&) {
                           // Title may have changed (first "# " line) and new notes must appear.
                           RenderLock lock(*this);
                           loadNotes();
                           if (selectorIndex > notes.size()) selectorIndex = notes.size();
                         });
}

void NotesListActivity::deleteNoteAt(const size_t noteIndex) {
  if (noteIndex >= notes.size()) return;
  const std::string fileName = notes[noteIndex].fileName;
  const std::string fullPath = std::string(kNotesDir) + "/" + fileName;

  // Confirm dialog is REQUIRED before delete (same pattern as FileBrowserActivity).
  startActivityForResult(
      std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_DELETE) + std::string("? "), fileName),
      [this, fullPath](const ActivityResult& res) {
        if (!res.isCancelled) {
          RenderLock lock(*this);
          clearBookCache(fullPath);  // the note may have been opened in the MD reader at some point
          if (!Storage.remove(fullPath.c_str())) {
            LOG_ERR("NOTES", "failed to delete %s", fullPath.c_str());
            lock.unlock();
            activityManager.goToFullScreenMessage("Delete FAILED - check SD card", EpdFontFamily::BOLD);
            return;
          }
          LOG_INF("NOTES", "deleted %s", fullPath.c_str());
          loadNotes();
          if (selectorIndex > notes.size()) selectorIndex = notes.size();
        }
        requestUpdate();
      });
}

void NotesListActivity::loop() {
  const int itemCount = static_cast<int>(notes.size()) + 1;  // + "+ New note"

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (lockNextConfirmRelease) {
      lockNextConfirmRelease = false;
      return;
    }
    if (selectorIndex == 0) {
      createNewNote();
    } else if (mappedInput.getHeldTime() >= kDeleteHoldMs) {
      deleteNoteAt(selectorIndex - 1);
    } else {
      const auto& e = notes[selectorIndex - 1];
      openEditor(e.fileName, e.title);
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoHome();
    return;
  }

  buttonNavigator.onNextRelease([this, itemCount] {
    selectorIndex = ButtonNavigator::nextIndex(static_cast<int>(selectorIndex), itemCount);
    requestUpdate();
  });
  buttonNavigator.onPreviousRelease([this, itemCount] {
    selectorIndex = ButtonNavigator::previousIndex(static_cast<int>(selectorIndex), itemCount);
    requestUpdate();
  });

  const int pageItems = UITheme::getNumberOfItemsPerPage(renderer, true, false, true, true);
  buttonNavigator.onNextContinuous([this, itemCount, pageItems] {
    selectorIndex = ButtonNavigator::nextPageIndex(static_cast<int>(selectorIndex), itemCount, pageItems);
    requestUpdate();
  });
  buttonNavigator.onPreviousContinuous([this, itemCount, pageItems] {
    selectorIndex = ButtonNavigator::previousPageIndex(static_cast<int>(selectorIndex), itemCount, pageItems);
    requestUpdate();
  });
}

void NotesListActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  char countStr[24];
  snprintf(countStr, sizeof(countStr), "%u note%s", (unsigned)notes.size(), notes.size() == 1 ? "" : "s");
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "Notes", countStr);

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

  if (loadFailed) {
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, contentTop + 20,
                      "SD ERROR: cannot open /Notes");
  } else {
    GUI.drawList(
        renderer, Rect{0, contentTop, pageWidth, contentHeight}, static_cast<int>(notes.size()) + 1,
        static_cast<int>(selectorIndex),
        [this](int index) { return index == 0 ? std::string("+ New note") : notes[index - 1].title; },
        [this](int index) { return index == 0 ? std::string() : notes[index - 1].fileName; },
        [](int index) { return index == 0 ? UIIcon::File : UIIcon::Notes; });
  }

  // Same hint text as EpubReaderBookmarksActivity — the confirm button is labelled "Open" here,
  // and the device has no button named "OK".
  GUI.drawHelpText(renderer,
                   Rect{0, pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing - 15, pageWidth, 20},
                   tr(STR_HOLD_OPEN_TO_DELETE));

  const char* confirmLabel = selectorIndex == 0 ? "New" : tr(STR_OPEN);
  const auto labels = mappedInput.mapLabels(tr(STR_HOME), confirmLabel, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}

#endif  // ENABLE_BLE_KEYBOARD
