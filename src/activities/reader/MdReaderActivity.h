#pragma once

#include <Epub/markdown/MarkdownSection.h>

#include <memory>
#include <string>

#include "CrossPointSettings.h"
#include "activities/Activity.h"

class Page;

// MdReaderActivity
// ----------------
// Native Markdown reader. Mirrors TxtReaderActivity's lifecycle (page turns, BACK long-press to
// file browser, progress save/restore, recent-books, status bar) but owns a MarkdownSection and
// renders each page through Page::render(...) exactly like EpubReaderActivity — reusing the shared
// layout/box render core rather than TxtReader's manual line drawing.
class MdReaderActivity final : public Activity {
  std::string mdPath;
  std::string cacheBasePath = "/.crosspoint";
  std::string cacheDir;          // /.crosspoint/md_<hash>
  std::string sectionCachePath;  // <cacheDir>/section.bin

  std::unique_ptr<MarkdownSection> section = nullptr;
  int nextPageNumber = 0;  // restore target, applied once the section is built/loaded
  int pagesUntilFullRefresh = 0;
  bool truncatedNoticePending = false;  // set when the parser stopped early (fail-loud marker)

  void renderContents(std::unique_ptr<Page> page, int orientedMarginTop, int orientedMarginRight,
                      int orientedMarginBottom, int orientedMarginLeft);
  void onOpenToc();  // Confirm: open the "#"/"##" table-of-contents selector and jump to a heading
  void renderStatusBar() const;
  void saveProgress() const;
  void loadProgress();
  [[nodiscard]] std::string getTitle() const;

 public:
  explicit MdReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string mdPath);
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return true; }
  ScreenshotInfo getScreenshotInfo() const override;
};
