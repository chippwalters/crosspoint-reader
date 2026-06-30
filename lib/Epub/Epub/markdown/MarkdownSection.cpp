#include "MarkdownSection.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Serialization.h>

#include <vector>

#include "Epub/Page.h"
#include "Epub/markdown/MarkdownToBoxParser.h"

namespace {
constexpr const char* TAG = "MDS";
// native-Markdown section cache. Bump to invalidate older caches.
// v2: anchor map records the heading level (u8) after each title+page, for TOC indentation.
constexpr uint8_t MD_SECTION_FILE_VERSION = 2;

// Header layout (in order):
//   version(u8) fontId(int) lineCompression(float) extraParagraphSpacing(bool)
//   paragraphAlignment(u8) viewportWidth(u16) viewportHeight(u16) hyphenationEnabled(bool)
//   focusReadingEnabled(bool) pageCount(u16) lutOffset(u32) anchorMapOffset(u32)
constexpr uint32_t HEADER_SIZE = sizeof(uint8_t) + sizeof(int) + sizeof(float) + sizeof(bool) + sizeof(uint8_t) +
                                 sizeof(uint16_t) + sizeof(uint16_t) + sizeof(bool) + sizeof(bool) + sizeof(uint16_t) +
                                 sizeof(uint32_t) + sizeof(uint32_t);
}  // namespace

uint32_t MarkdownSection::onPageComplete(std::unique_ptr<Page> page) {
  if (!file) {
    LOG_ERR(TAG, "File not open for writing page %d", pageCount);
    return 0;
  }
  const uint32_t position = file.position();
  if (!page->serialize(file)) {
    LOG_ERR(TAG, "Failed to serialize page %d", pageCount);
    return 0;
  }
  pageCount++;
  return position;
}

void MarkdownSection::writeSectionFileHeader(const int fontId, const float lineCompression,
                                             const bool extraParagraphSpacing, const uint8_t paragraphAlignment,
                                             const uint16_t viewportWidth, const uint16_t viewportHeight,
                                             const bool hyphenationEnabled, const bool focusReadingEnabled) {
  if (!file) {
    LOG_DBG(TAG, "File not open for writing header");
    return;
  }
  static_assert(HEADER_SIZE == sizeof(MD_SECTION_FILE_VERSION) + sizeof(fontId) + sizeof(lineCompression) +
                                   sizeof(extraParagraphSpacing) + sizeof(paragraphAlignment) + sizeof(viewportWidth) +
                                   sizeof(viewportHeight) + sizeof(hyphenationEnabled) + sizeof(focusReadingEnabled) +
                                   sizeof(pageCount) + sizeof(uint32_t) + sizeof(uint32_t),
                "Header size mismatch");
  serialization::writePod(file, MD_SECTION_FILE_VERSION);
  serialization::writePod(file, fontId);
  serialization::writePod(file, lineCompression);
  serialization::writePod(file, extraParagraphSpacing);
  serialization::writePod(file, paragraphAlignment);
  serialization::writePod(file, viewportWidth);
  serialization::writePod(file, viewportHeight);
  serialization::writePod(file, hyphenationEnabled);
  serialization::writePod(file, focusReadingEnabled);
  serialization::writePod(file, pageCount);                 // patched later
  serialization::writePod(file, static_cast<uint32_t>(0));  // LUT offset (patched later)
  serialization::writePod(file, static_cast<uint32_t>(0));  // anchor map offset (patched later)
}

bool MarkdownSection::loadSectionFile(const int fontId, const float lineCompression, const bool extraParagraphSpacing,
                                      const uint8_t paragraphAlignment, const uint16_t viewportWidth,
                                      const uint16_t viewportHeight, const bool hyphenationEnabled,
                                      const bool focusReadingEnabled) {
  if (!Storage.openFileForRead(TAG, filePath, file)) {
    return false;
  }

  uint8_t version;
  serialization::readPod(file, version);
  if (version != MD_SECTION_FILE_VERSION) {
    file.close();
    LOG_ERR(TAG, "Cache version mismatch %u", version);
    clearCache();
    return false;
  }

  int fileFontId;
  float fileLineCompression;
  bool fileExtraParagraphSpacing;
  uint8_t fileParagraphAlignment;
  uint16_t fileViewportWidth, fileViewportHeight;
  bool fileHyphenationEnabled, fileFocusReadingEnabled;
  serialization::readPod(file, fileFontId);
  serialization::readPod(file, fileLineCompression);
  serialization::readPod(file, fileExtraParagraphSpacing);
  serialization::readPod(file, fileParagraphAlignment);
  serialization::readPod(file, fileViewportWidth);
  serialization::readPod(file, fileViewportHeight);
  serialization::readPod(file, fileHyphenationEnabled);
  serialization::readPod(file, fileFocusReadingEnabled);

  if (fontId != fileFontId || lineCompression != fileLineCompression ||
      extraParagraphSpacing != fileExtraParagraphSpacing || paragraphAlignment != fileParagraphAlignment ||
      viewportWidth != fileViewportWidth || viewportHeight != fileViewportHeight ||
      hyphenationEnabled != fileHyphenationEnabled || focusReadingEnabled != fileFocusReadingEnabled) {
    file.close();
    LOG_ERR(TAG, "Cache parameters do not match");
    clearCache();
    return false;
  }

  serialization::readPod(file, pageCount);
  file.close();
  LOG_DBG(TAG, "Loaded markdown section: %d pages", pageCount);
  return true;
}

bool MarkdownSection::clearCache() const {
  if (!Storage.exists(filePath.c_str())) {
    return true;
  }
  if (!Storage.remove(filePath.c_str())) {
    LOG_ERR(TAG, "Failed to clear cache");
    return false;
  }
  return true;
}

bool MarkdownSection::createSectionFile(const int fontId, const float lineCompression, const bool extraParagraphSpacing,
                                        const uint8_t paragraphAlignment, const uint16_t viewportWidth,
                                        const uint16_t viewportHeight, const bool hyphenationEnabled,
                                        const bool focusReadingEnabled, const std::function<bool()>& shouldAbort) {
  // Ensure the cache directory exists.
  const size_t slash = filePath.find_last_of('/');
  if (slash != std::string::npos) {
    Storage.mkdir(filePath.substr(0, slash).c_str());
  }

  if (!Storage.openFileForWrite(TAG, filePath, file)) {
    return false;
  }
  pageCount = 0;
  writeSectionFileHeader(fontId, lineCompression, extraParagraphSpacing, paragraphAlignment, viewportWidth,
                         viewportHeight, hyphenationEnabled, focusReadingEnabled);

  std::vector<uint32_t> lut;
  MarkdownToBoxParser parser(
      fontId, lineCompression, extraParagraphSpacing, paragraphAlignment, viewportWidth, viewportHeight,
      hyphenationEnabled, focusReadingEnabled, renderer,
      [this, &lut](std::unique_ptr<Page> page, uint16_t, uint16_t) { lut.push_back(onPageComplete(std::move(page))); },
      shouldAbort);

  const bool ok = parser.parseFile(mdPath);
  truncated = parser.hitMemoryLimit() || parser.aborted();
  if (truncated) {
    LOG_ERR(TAG, "Markdown parse stopped early (truncated=%d) — partial cache", truncated);
  }
  if (!ok) {
    LOG_ERR(TAG, "Failed to parse markdown and build pages");
    file.close();
    Storage.remove(filePath.c_str());
    return false;
  }

  // Page-offset LUT.
  const uint32_t lutOffset = file.position();
  for (const uint32_t offset : lut) {
    if (offset == 0) {
      LOG_ERR(TAG, "Invalid page position in LUT");
      file.close();
      Storage.remove(filePath.c_str());
      return false;
    }
    serialization::writePod(file, offset);
  }

  // Anchor map (header text -> page) for "#"/"##" TOC navigation.
  const uint32_t anchorMapOffset = file.position();
  const auto& anchors = parser.getAnchors();
  serialization::writePod(file, static_cast<uint16_t>(anchors.size()));
  for (const auto& a : anchors) {
    serialization::writeString(file, a.title);
    serialization::writePod(file, a.page);
    serialization::writePod(file, a.level);
  }

  // Patch header: pageCount, lutOffset, anchorMapOffset.
  file.seek(HEADER_SIZE - sizeof(uint32_t) * 2 - sizeof(pageCount));
  serialization::writePod(file, pageCount);
  serialization::writePod(file, lutOffset);
  serialization::writePod(file, anchorMapOffset);
  file.close();
  LOG_DBG(TAG, "Built markdown section: %d pages, %u TOC anchors", pageCount, static_cast<unsigned>(anchors.size()));
  return true;
}

std::unique_ptr<Page> MarkdownSection::loadPageFromSectionFile() {
  if (!Storage.openFileForRead(TAG, filePath, file)) {
    return nullptr;
  }
  file.seek(HEADER_SIZE - sizeof(uint32_t) * 2);
  uint32_t lutOffset;
  serialization::readPod(file, lutOffset);
  file.seek(lutOffset + sizeof(uint32_t) * currentPage);
  uint32_t pagePos;
  serialization::readPod(file, pagePos);
  file.seek(pagePos);
  auto page = Page::deserialize(file);
  file.close();
  return page;
}

std::optional<uint16_t> MarkdownSection::getCachedPageCount() const {
  HalFile f;
  if (!Storage.openFileForRead(TAG, filePath, f)) {
    return std::nullopt;
  }
  if (f.size() < HEADER_SIZE) {
    return std::nullopt;
  }
  f.seek(HEADER_SIZE - sizeof(uint32_t) * 2 - sizeof(uint16_t));
  uint16_t count;
  serialization::readPod(f, count);
  return count;
}

std::optional<uint16_t> MarkdownSection::getPageForAnchor(const std::string& anchor) const {
  HalFile f;
  if (!Storage.openFileForRead(TAG, filePath, f)) {
    return std::nullopt;
  }
  const uint32_t fileSize = f.size();
  f.seek(HEADER_SIZE - sizeof(uint32_t));
  uint32_t anchorMapOffset;
  serialization::readPod(f, anchorMapOffset);
  if (anchorMapOffset == 0 || anchorMapOffset >= fileSize) {
    return std::nullopt;
  }
  f.seek(anchorMapOffset);
  uint16_t count;
  serialization::readPod(f, count);
  for (uint16_t i = 0; i < count; i++) {
    std::string key;
    uint16_t page;
    uint8_t level;
    serialization::readString(f, key);
    serialization::readPod(f, page);
    serialization::readPod(f, level);
    if (key == anchor) {
      return page;
    }
  }
  return std::nullopt;
}

std::vector<MdTocEntry> MarkdownSection::readAnchors() const {
  std::vector<MdTocEntry> entries;
  HalFile f;
  if (!Storage.openFileForRead(TAG, filePath, f)) {
    return entries;
  }
  const uint32_t fileSize = f.size();
  f.seek(HEADER_SIZE - sizeof(uint32_t));
  uint32_t anchorMapOffset;
  serialization::readPod(f, anchorMapOffset);
  if (anchorMapOffset == 0 || anchorMapOffset >= fileSize) {
    return entries;
  }
  f.seek(anchorMapOffset);
  uint16_t count;
  serialization::readPod(f, count);
  entries.reserve(count);
  for (uint16_t i = 0; i < count; i++) {
    MdTocEntry e;
    serialization::readString(f, e.title);
    serialization::readPod(f, e.page);
    serialization::readPod(f, e.level);
    entries.push_back(std::move(e));
  }
  return entries;
}
