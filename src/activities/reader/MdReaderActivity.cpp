#include "MdReaderActivity.h"

#include <Epub/Page.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Memory.h>

#include <functional>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "ProgressFile.h"
#include "ReaderUtils.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr const char* TAG = "MDR";
}  // namespace

MdReaderActivity::MdReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string mdPath)
    : Activity("MdReader", renderer, mappedInput), mdPath(std::move(mdPath)) {
  // Cache key mirrors Txt/Epub: a per-file hashed directory under /.crosspoint.
  const size_t hash = std::hash<std::string>{}(this->mdPath);
  cacheDir = cacheBasePath + "/md_" + std::to_string(hash);
  sectionCachePath = cacheDir + "/section.bin";
}

std::string MdReaderActivity::getTitle() const {
  const size_t lastSlash = mdPath.find_last_of('/');
  std::string name = (lastSlash != std::string::npos) ? mdPath.substr(lastSlash + 1) : mdPath;
  // Strip a trailing ".md" (case-insensitive) for display.
  if (name.size() >= 3) {
    const std::string ext = name.substr(name.size() - 3);
    if (ext == ".md" || ext == ".MD" || ext == ".Md" || ext == ".mD") {
      name = name.substr(0, name.size() - 3);
    }
  }
  return name;
}

void MdReaderActivity::onEnter() {
  Activity::onEnter();

  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);

  // Ensure the cache directory exists (MarkdownSection also mkdirs its parent, but progress.bin
  // is written here independently of pagination).
  if (!Storage.exists(cacheBasePath.c_str())) {
    Storage.mkdir(cacheBasePath.c_str());
  }
  if (!Storage.exists(cacheDir.c_str())) {
    Storage.mkdir(cacheDir.c_str());
  }

  // Save current file as last opened and add to recent books.
  const auto fileName = mdPath.substr(mdPath.rfind('/') + 1);
  APP_STATE.openEpubPath = mdPath;
  APP_STATE.saveToFile();
  RECENT_BOOKS.addBook(mdPath, fileName, "", "");

  loadProgress();

  requestUpdate();
}

void MdReaderActivity::onExit() {
  Activity::onExit();

  // Reset orientation back to portrait for the rest of the UI.
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  APP_STATE.readerActivityLoadCount = 0;
  APP_STATE.saveToFile();

  section.reset();
}

void MdReaderActivity::loop() {
  // Long press BACK (1s+) goes to file selection.
  if (mappedInput.isPressed(MappedInputManager::Button::Back) && mappedInput.getHeldTime() >= ReaderUtils::GO_HOME_MS) {
    activityManager.goToFileBrowser(mdPath);
    return;
  }

  // Short press BACK goes directly to home.
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() < ReaderUtils::GO_HOME_MS) {
    onGoHome();
    return;
  }

  const auto [prevTriggered, nextTriggered, fromTilt] = ReaderUtils::detectPageTurn(mappedInput);
  if (!prevTriggered && !nextTriggered) {
    return;
  }

  // No current section yet (or it failed to build): trigger a (re)render.
  if (!section) {
    requestUpdate();
    return;
  }

  if (prevTriggered) {
    if (section->currentPage > 0) {
      section->currentPage--;
      requestUpdate();
    }
  } else {  // nextTriggered
    if (section->currentPage < section->pageCount - 1) {
      section->currentPage++;
      requestUpdate();
    } else {
      onGoHome();
    }
  }
}

void MdReaderActivity::render(RenderLock&&) {
  // Apply screen viewable areas plus configured margins and the status bar reservation.
  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);
  orientedMarginTop += SETTINGS.screenMargin;
  orientedMarginLeft += SETTINGS.screenMargin;
  orientedMarginRight += SETTINGS.screenMargin;
  orientedMarginBottom += std::max(SETTINGS.screenMargin,
                                   static_cast<uint8_t>(UITheme::getInstance().getStatusBarHeight()));

  const uint16_t viewportWidth = renderer.getScreenWidth() - orientedMarginLeft - orientedMarginRight;
  const uint16_t viewportHeight = renderer.getScreenHeight() - orientedMarginTop - orientedMarginBottom;

  if (!section) {
    section = makeUniqueNoThrow<MarkdownSection>(mdPath, sectionCachePath, renderer);
    if (!section) {
      LOG_ERR(TAG, "Failed to allocate MarkdownSection");
      return;
    }

    if (!section->loadSectionFile(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                                  SETTINGS.extraParagraphSpacing, SETTINGS.paragraphAlignment, viewportWidth,
                                  viewportHeight, SETTINGS.hyphenationEnabled, SETTINGS.focusReadingEnabled)) {
      LOG_DBG(TAG, "Cache not found, paginating markdown...");

      GUI.drawPopup(renderer, tr(STR_INDEXING));

      if (!section->createSectionFile(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                                      SETTINGS.extraParagraphSpacing, SETTINGS.paragraphAlignment, viewportWidth,
                                      viewportHeight, SETTINGS.hyphenationEnabled, SETTINGS.focusReadingEnabled)) {
        LOG_ERR(TAG, "Failed to paginate markdown");
        section.reset();
        return;
      }
    }

    // Fail loud: parser stopped early on low heap → surface a truncation notice.
    truncatedNoticePending = section->truncated;

    // Restore saved page, clamped to the freshly computed page count.
    section->currentPage = nextPageNumber;
    if (section->currentPage < 0) {
      section->currentPage = 0;
    } else if (section->pageCount > 0 && section->currentPage >= section->pageCount) {
      section->currentPage = section->pageCount - 1;
    }
  }

  renderer.clearScreen();

  if (section->pageCount == 0) {
    LOG_DBG(TAG, "No pages to render");
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_EMPTY_FILE), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  if (section->currentPage < 0 || section->currentPage >= section->pageCount) {
    section->currentPage = 0;
  }

  auto page = section->loadPageFromSectionFile();
  if (!page) {
    LOG_ERR(TAG, "Failed to load page from cache - clearing section cache");
    section->clearCache();
    section.reset();
    requestUpdate();  // Retry after clearing the (possibly corrupt) cache.
    return;
  }

  renderContents(std::move(page), orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft);

  saveProgress();

  if (truncatedNoticePending) {
    truncatedNoticePending = false;
    // Fail-loud marker for partial content (low-heap early stop). Literal to avoid touching the
    // auto-generated i18n tables; promote to a STR_ key when the i18n set is regenerated.
    GUI.drawPopup(renderer, "Content truncated");
  }
}

// Mirrors EpubReaderActivity::renderContents — renders the page through the shared box render core
// with font prewarm, the BW/refresh cycle, and the grayscale (anti-alias / image) passes.
void MdReaderActivity::renderContents(std::unique_ptr<Page> page, const int orientedMarginTop,
                                      const int orientedMarginRight, const int orientedMarginBottom,
                                      const int orientedMarginLeft) {
  const int fontId = SETTINGS.getReaderFontId();

  // Font prewarm: scan pass accumulates text, then prewarm, then real render.
  auto* fcm = renderer.getFontCacheManager();
  auto scope = fcm->createPrewarmScope();
  page->render(renderer, fontId, orientedMarginLeft, orientedMarginTop);  // scan pass
  scope.endScanAndPrewarm();

  const bool pageHasImages = page->hasImages();
  const bool needsTextGrayscale = SETTINGS.textAntiAliasing;
  const bool needsAnyGrayscale = needsTextGrayscale || pageHasImages;
  auto renderGrayscalePass = [&]() {
    if (needsTextGrayscale) {
      page->render(renderer, fontId, orientedMarginLeft, orientedMarginTop);
    } else {
      page->renderImages(renderer, fontId, orientedMarginLeft, orientedMarginTop);
    }
  };

  page->render(renderer, fontId, orientedMarginLeft, orientedMarginTop);
  renderStatusBar();

  if (pageHasImages) {
    int16_t imgX, imgY, imgW, imgH;
    if (page->getImageBoundingBox(imgX, imgY, imgW, imgH)) {
      renderer.fillRect(imgX + orientedMarginLeft, imgY + orientedMarginTop, imgW, imgH, false);
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
      page->render(renderer, fontId, orientedMarginLeft, orientedMarginTop);
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    } else {
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    }
    pagesUntilFullRefresh = 1;
  } else {
    ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);
  }

  if (needsAnyGrayscale && renderer.supportsStripGrayscale()) {
    constexpr int STRIP_ROWS = 80;
    const int gh = renderer.getDisplayHeight();
    const int gwBytes = renderer.getDisplayWidthBytes();

    auto scratch = makeUniqueNoThrow<uint8_t[]>(static_cast<size_t>(gwBytes) * STRIP_ROWS);
    if (!scratch) {
      LOG_ERR(TAG, "OOM: grayscale strip scratch (%d bytes); skipping AA this page", gwBytes * STRIP_ROWS);
    } else {
      renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
      for (int y = 0; y < gh; y += STRIP_ROWS) {
        const int rows = (gh - y < STRIP_ROWS) ? (gh - y) : STRIP_ROWS;
        renderer.beginStripTarget(scratch.get(), y, rows);
        renderer.clearScreen(0x00);
        renderGrayscalePass();
        renderer.endStripTarget();
        renderer.writeGrayscalePlaneStrip(true, scratch.get(), y, rows);
      }

      renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
      for (int y = 0; y < gh; y += STRIP_ROWS) {
        const int rows = (gh - y < STRIP_ROWS) ? (gh - y) : STRIP_ROWS;
        renderer.beginStripTarget(scratch.get(), y, rows);
        renderer.clearScreen(0x00);
        renderGrayscalePass();
        renderer.endStripTarget();
        renderer.writeGrayscalePlaneStrip(false, scratch.get(), y, rows);
      }

      renderer.setRenderMode(GfxRenderer::BW);
      renderer.displayGrayBuffer();
      renderer.cleanupGrayscaleWithFrameBuffer();
    }
  } else if (needsAnyGrayscale) {
    if (!renderer.storeBwBuffer()) {
      LOG_ERR(TAG, "Failed to store BW buffer for grayscale render; skipping grayscale this page");
      return;
    }
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
    renderGrayscalePass();
    renderer.copyGrayscaleLsbBuffers();

    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
    renderGrayscalePass();
    renderer.copyGrayscaleMsbBuffers();

    renderer.displayGrayBuffer();
    renderer.setRenderMode(GfxRenderer::BW);
    renderer.restoreBwBuffer();
  }
}

void MdReaderActivity::renderStatusBar() const {
  if (!section) {
    return;
  }
  const int totalPages = section->pageCount;
  const int currentPage = section->currentPage + 1;
  const float progress = totalPages > 0 ? currentPage * 100.0f / totalPages : 0;
  std::string title;
  if (SETTINGS.statusBarTitle != CrossPointSettings::STATUS_BAR_TITLE::HIDE_TITLE) {
    title = getTitle();
  }
  GUI.drawStatusBar(renderer, progress, currentPage, totalPages, title);
}

void MdReaderActivity::saveProgress() const {
  if (!section) {
    return;
  }
  const int currentPage = section->currentPage;
  uint8_t data[4];
  data[0] = currentPage & 0xFF;
  data[1] = (currentPage >> 8) & 0xFF;
  data[2] = 0;
  data[3] = 0;
  if (!ProgressFile::writeAtomic(cacheDir, data, sizeof(data))) {
    LOG_ERR(TAG, "Failed to save progress: page %d", currentPage);
  }
}

void MdReaderActivity::loadProgress() {
  HalFile f;
  if (Storage.openFileForRead(TAG, cacheDir + "/progress.bin", f)) {
    uint8_t data[4];
    if (f.read(data, 4) == 4) {
      nextPageNumber = data[0] + (data[1] << 8);
      if (nextPageNumber < 0) {
        nextPageNumber = 0;
      }
      LOG_DBG(TAG, "Loaded progress: page %d", nextPageNumber);
    }
  }
}

ScreenshotInfo MdReaderActivity::getScreenshotInfo() const {
  ScreenshotInfo info;
  info.readerType = ScreenshotInfo::ReaderType::Markdown;
  const std::string t = getTitle();
  snprintf(info.title, sizeof(info.title), "%s", t.c_str());
  if (section) {
    info.currentPage = section->currentPage + 1;
    info.totalPages = section->pageCount;
    info.progressPercent =
        section->pageCount > 0 ? static_cast<int>((section->currentPage + 1) * 100.0f / section->pageCount + 0.5f) : 0;
    if (info.progressPercent > 100) info.progressPercent = 100;
  }
  return info;
}
