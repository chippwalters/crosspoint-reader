// NotesListActivity — the "Notes" home-tile screen (BLE-keyboard typewriter feature).
//
// Lists /Notes/*.md from SD newest-first (timestamp filenames sort lexicographically; see .cpp),
// with a "+ New note" item pinned at the top. Display title = first "# " heading line of the
// file if present (scanned in the first few hundred bytes), else the filename.
//  - Select "+ New note": creates /Notes/YYYY-MM-DD HHMM.md (auto-timestamp; sequential
//    "Note NNNN.md" fallback when the device has no valid wall clock — X4 has no RTC date) and
//    opens the editor.
//  - Select a note: opens the editor appended at the end of the file.
//  - HOLD Confirm (≥1 s, same gesture as the file browser): delete, behind a ConfirmationActivity.
//
// No BLE runs on this screen — the radio comes up only inside the editor / pairing screens.
// Only compiled under ENABLE_BLE_KEYBOARD (empty TU otherwise).
#pragma once

#ifdef ENABLE_BLE_KEYBOARD

#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class NotesListActivity final : public Activity {
 public:
  explicit NotesListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("NotesList", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  struct NoteEntry {
    std::string fileName;  // name only, no directory
    std::string title;     // display title (heading or filename stem)
  };

  void loadNotes();
  void openEditor(const std::string& fileName, const std::string& title);
  void createNewNote();
  void deleteNoteAt(size_t noteIndex);
  static std::string extractTitle(const std::string& fullPath, const std::string& fileName);
  std::string makeNewNoteFileName() const;

  ButtonNavigator buttonNavigator;
  std::vector<NoteEntry> notes;
  size_t selectorIndex = 0;  // 0 = "+ New note", 1.. = notes[i-1]
  bool lockNextConfirmRelease = false;
  bool loadFailed = false;
};

#endif  // ENABLE_BLE_KEYBOARD
