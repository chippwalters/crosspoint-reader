#pragma once

#include <cstdint>
#include <vector>

#include "Epub/markdown/MarkdownSection.h"  // MdTocEntry
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// Table-of-contents selector for the native Markdown reader. Lists every "#"/"##" heading
// (indented by level) and returns the selected heading's page via PageResult; Back cancels.
// Mirrors EpubReaderChapterSelectionActivity, but for MarkdownSection's anchor map.
class MdReaderTocActivity final : public Activity {
  std::vector<MdTocEntry> entries;
  int currentPage;  // the reader's current page, used to preselect the active heading
  ButtonNavigator buttonNavigator;
  int selectorIndex = 0;

 public:
  explicit MdReaderTocActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                               std::vector<MdTocEntry> entries, const int currentPage)
      : Activity("MdReaderToc", renderer, mappedInput), entries(std::move(entries)), currentPage(currentPage) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
