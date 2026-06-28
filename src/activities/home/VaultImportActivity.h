#pragma once
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

/**
 * Recursively scans the whole SD card for *.md files and presents them as one
 * flat list (filename + source folder). Selecting a file returns its full path
 * to the caller via FilePathResult; the vault then encrypts it and deletes the
 * original (a move into the vault). Back cancels.
 */
class VaultImportActivity final : public Activity {
  ButtonNavigator buttonNavigator;
  int selectedIndex = 0;
  std::vector<std::string> paths;   // full paths to .md files
  std::vector<std::string> labels;  // basenames for display
  std::vector<std::string> dirs;    // parent folder for each, as subtitle

  void scan();

 public:
  VaultImportActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("VaultImport", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
