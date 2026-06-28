#pragma once
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

/**
 * In-memory viewer for a decrypted vault note.
 *
 * Receives the already-decrypted plaintext (no key, no file access) and
 * paginates it entirely in RAM — it never reads from or writes to the SD card,
 * never touches Recents/progress state, and wipes its plaintext buffer on exit.
 * Rendering is plain text in a built-in UI font (basic Markdown styling is a
 * later increment).
 */
class VaultReaderActivity final : public Activity {
  std::string content;  // decrypted plaintext, wiped on exit
  std::string title;

  ButtonNavigator buttonNavigator;
  std::vector<std::string> lines;  // wrapped display lines
  bool initialized = false;
  int currentPage = 0;
  int totalPages = 1;
  int linesPerPage = 1;
  int marginLeft = 0;
  int marginTop = 0;

  void initialize();
  void wrapInto(const std::string& paragraph, int viewportWidth, int fontId);

 public:
  VaultReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string content, std::string title)
      : Activity("VaultReader", renderer, mappedInput), content(std::move(content)), title(std::move(title)) {}
  ~VaultReaderActivity() override;

  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
