#pragma once

#include <HalStorage.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>

class Page;
class GfxRenderer;

// MarkdownSection
// ---------------
// Analog of Section, for native Markdown. Drives MarkdownToBoxParser to paginate a .md
// file, serializing each Page to a section-cache .bin (Page::serialize) with a page-offset
// LUT and an anchor map (the "#"/"##" header -> page table), plus a validation header keyed
// on version/font/spacing/alignment/viewport. Same serialize-and-free-per-page transient as
// Section, fast page turns, and getPageForAnchor() TOC navigation.
class MarkdownSection {
  GfxRenderer& renderer;
  std::string mdPath;    // source .md on storage
  std::string filePath;  // cache .bin path
  HalFile file;

  void writeSectionFileHeader(int fontId, float lineCompression, bool extraParagraphSpacing, uint8_t paragraphAlignment,
                              uint16_t viewportWidth, uint16_t viewportHeight, bool hyphenationEnabled,
                              bool focusReadingEnabled);
  uint32_t onPageComplete(std::unique_ptr<Page> page);

 public:
  uint16_t pageCount = 0;
  int currentPage = 0;
  bool truncated = false;  // set when the parser stopped early (low heap) — caller shows a marker

  explicit MarkdownSection(std::string mdPath, std::string cacheFilePath, GfxRenderer& renderer)
      : renderer(renderer), mdPath(std::move(mdPath)), filePath(std::move(cacheFilePath)) {}
  ~MarkdownSection() = default;

  bool loadSectionFile(int fontId, float lineCompression, bool extraParagraphSpacing, uint8_t paragraphAlignment,
                       uint16_t viewportWidth, uint16_t viewportHeight, bool hyphenationEnabled,
                       bool focusReadingEnabled);
  bool createSectionFile(int fontId, float lineCompression, bool extraParagraphSpacing, uint8_t paragraphAlignment,
                         uint16_t viewportWidth, uint16_t viewportHeight, bool hyphenationEnabled,
                         bool focusReadingEnabled, const std::function<bool()>& shouldAbort = nullptr);
  bool clearCache() const;
  std::unique_ptr<Page> loadPageFromSectionFile();

  // Look up the page number for an anchor (header text) from the section cache file.
  std::optional<uint16_t> getPageForAnchor(const std::string& anchor) const;
  // Get the page count from the cache header without fully loading it.
  std::optional<uint16_t> getCachedPageCount() const;
};
