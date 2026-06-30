#pragma once

#include <EpdFontFamily.h>

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "Epub/ParsedText.h"
#include "Epub/blocks/BlockStyle.h"
#include "Epub/blocks/TextBlock.h"
#include "Epub/markdown/md_parser.h"

class Page;
class GfxRenderer;

// MarkdownToBoxParser
// -------------------
// Thin front-end that drives the vendored streaming Markdown tokenizer (md_parser.c,
// papyrix MIT) and emits into OUR existing box model (ParsedText / TextBlock / Page),
// so .md pages render identically to EPUB/TXT. Modeled on ChapterHtmlSlimParser: same
// render params, same partWordBuffer/style-stack/addLineToPage discipline.
//
// Token -> box mapping (ROUTE-A-BUILD-PLAN.md):
//   MD_TEXT            -> accumulate word (cap kMaxWordSize) -> ParsedText::addWord
//   MD_HEADER_*        -> bold + centered block + record TOC anchor
//   bold/italic/strike -> EpdFontFamily style bits
//   MD_LIST_ITEM_*     -> hanging-indent block + bullet/number
//   code (inline/block)-> mono placeholder == italic (no mono face yet, SPEC s4.0)
//   MD_HR              -> PageHorizontalRule (mirror ChapterHtmlSlimParser::emitHorizontalRule)
//   blockquote         -> indented italic block
//   link               -> underlined text + URL via Page::addFootnote
//   image              -> "[Image: alt]" italic placeholder (SPEC s4.2, ImageBlock later)
//   NEWLINE/blank line -> flush word / paragraph break -> layoutAndExtractLines
//
// Memory: streams in kReadChunkSize storage reads, no DOM, arena via ParsedText, hard
// caps + tripwires re-fit to the ~128 KB C3 working heap (see ROUTE-A-MEMORY.md).
class MarkdownToBoxParser {
 public:
  // ---- Memory constants (cite: ROUTE-A-MEMORY.md "A. Memory techniques") ----
  // Word cap: auto-flush; matches ChapterHtmlSlimParser.h:22 MAX_WORD_SIZE.
  static constexpr int kMaxWordSize = 200;
  // Line buffer (member, not heap). Kept for parity with the papyrix line reader.
  static constexpr int kLineBufferSize = 512;
  // Per-block word cap: tuned DOWN from papyrix's 512 for the 128 KB budget.
  static constexpr size_t kMaxWordsPerBlock = 256;
  // Only start checking the block-split tripwire once a block is this large.
  static constexpr size_t kBlockSplitWordCheck = 300;
  // Block-split tripwire: split earlier than papyrix (25000) given our tighter heap.
  static constexpr size_t kBlockSplitMinFreeBlock = 32000;
  // Low-heap early-stop: stop the batch (fail loud, truncate) below this floor.
  // Softer than papyrix's 12000 because heavy render dips to ~43 KB free.
  static constexpr size_t kLowHeapEarlyStop = 20000;
  // Storage read chunk (see TxtReaderActivity.cpp CHUNK_SIZE).
  static constexpr size_t kReadChunkSize = 8 * 1024;
  // Abort-hook cadence (in newlines), per ROUTE-A-MEMORY.md.
  static constexpr uint16_t kAbortCheckEveryLines = 20;

  // page, paragraphIndex, listItemIndex (mirrors ChapterHtmlSlimParser/Section completePageFn)
  using CompletePageFn = std::function<void(std::unique_ptr<Page>, uint16_t, uint16_t)>;

  MarkdownToBoxParser(int fontId, float lineCompression, bool extraParagraphSpacing, uint8_t paragraphAlignment,
                      uint16_t viewportWidth, uint16_t viewportHeight, bool hyphenationEnabled,
                      bool focusReadingEnabled, GfxRenderer& renderer, CompletePageFn completePageFn,
                      std::function<bool()> shouldAbort = nullptr)
      : fontId(fontId),
        lineCompression(lineCompression),
        extraParagraphSpacing(extraParagraphSpacing),
        paragraphAlignment(paragraphAlignment),
        viewportWidth(viewportWidth),
        viewportHeight(viewportHeight),
        hyphenationEnabled(hyphenationEnabled),
        focusReadingEnabled(focusReadingEnabled),
        renderer(renderer),
        completePageFn(std::move(completePageFn)),
        shouldAbort(std::move(shouldAbort)) {}
  ~MarkdownToBoxParser() = default;

  // Parse + paginate the .md file at `path`. Returns false on open/alloc/parse failure.
  // Sets hitMemoryLimit()/aborted() if it stopped early; the caller renders what fits
  // and shows a "content truncated" marker (fail loud).
  bool parseFile(const std::string& path);

  bool hitMemoryLimit() const { return hitMemoryLimit_; }
  bool aborted() const { return aborted_; }
  const std::vector<std::pair<std::string, uint16_t>>& getAnchors() const { return anchorData; }

 private:
  // md_callback_t trampoline -> member dispatch
  static bool tokenTrampoline(const md_token_t* token, void* userData);
  bool onToken(const md_token_t* token);

  EpdFontFamily::Style currentStyle() const;
  void ensureBlock();
  void accumulateText(const char* text, uint16_t length);
  void flushWord();
  void flushBlock();
  void addLineToPage(const std::shared_ptr<TextBlock>& line);
  void startNewBlock(const BlockStyle& style);
  void emitHorizontalRule();
  void maybeSplitBlock();          // block-split tripwire at a safe (newline) boundary
  bool checkLowHeapStop();         // sets hitMemoryLimit_ and returns true when below floor
  BlockStyle paragraphStyle() const;
  BlockStyle headerStyle() const;
  BlockStyle indentedStyle() const;  // lists + blockquotes

  // ---- render params ----
  int fontId;
  float lineCompression;
  bool extraParagraphSpacing;
  uint8_t paragraphAlignment;
  uint16_t viewportWidth;
  uint16_t viewportHeight;
  bool hyphenationEnabled;
  bool focusReadingEnabled;
  GfxRenderer& renderer;
  CompletePageFn completePageFn;
  std::function<bool()> shouldAbort;

  // ---- working state ----
  std::unique_ptr<ParsedText> currentTextBlock = nullptr;
  std::unique_ptr<Page> currentPage = nullptr;
  int16_t currentPageNextY = 0;
  // build words from chars; auto-break if longer than this (leave room for NUL).
  char partWordBuffer[kMaxWordSize + 1] = {};
  int partWordBufferIndex = 0;

  // ---- effective inline/block style flags ----
  bool inHeader_ = false;
  bool inBold_ = false;
  bool inItalic_ = false;
  bool inStrike_ = false;
  bool inInlineCode_ = false;
  bool inCodeBlock_ = false;
  bool inBlockquote_ = false;
  bool inImageAlt_ = false;
  bool inLink_ = false;       // underline link display text
  bool inLinkText_ = false;   // accumulating link label
  std::string linkLabel_;

  // ---- header -> anchor (free TOC) ----
  std::string headerText_;
  uint16_t headerAnchorPage_ = 0;

  // ---- footnotes for links (drained onto pages in addLineToPage) ----
  std::vector<std::pair<std::string, std::string>> pendingFootnotes_;  // <label, href>

  // ---- anchor map + LUT counters (mirror ChapterHtmlSlimParser/Section) ----
  std::vector<std::pair<std::string, uint16_t>> anchorData;
  uint16_t completedPageCount = 0;
  uint16_t paragraphIndex = 0;
  uint16_t listItemIndex = 0;

  // ---- control ----
  bool hitMemoryLimit_ = false;
  bool aborted_ = false;
  bool sawTextSinceNewline_ = false;  // distinguishes soft line break from blank-line paragraph break
  uint16_t lineCounter_ = 0;
};
