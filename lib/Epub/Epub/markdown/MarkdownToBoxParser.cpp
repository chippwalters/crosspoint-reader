#include "MarkdownToBoxParser.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Utf8.h>
#include <esp_heap_caps.h>

#include <algorithm>
#include <cstring>
#include <new>

#include "Epub/Page.h"

namespace {
constexpr const char* TAG = "MDB";
bool isWhitespaceChar(const char c) { return c == ' ' || c == '\r' || c == '\n' || c == '\t'; }
constexpr const char* BULLET = "\xe2\x80\xa2";  // U+2022
}  // namespace

// -------- style --------
EpdFontFamily::Style MarkdownToBoxParser::currentStyle() const {
  uint8_t s = EpdFontFamily::REGULAR;
  if (inBold_ || inHeader_) s |= EpdFontFamily::BOLD;
  // No real monospace face yet (SPEC s4.0): render code in italic as a stand-in.
  if (inItalic_ || inInlineCode_ || inCodeBlock_ || inBlockquote_ || inImageAlt_) s |= EpdFontFamily::ITALIC;
  if (inStrike_) s |= EpdFontFamily::STRIKETHROUGH;
  if (inLink_) s |= EpdFontFamily::UNDERLINE;
  return static_cast<EpdFontFamily::Style>(s);
}

BlockStyle MarkdownToBoxParser::paragraphStyle() const {
  BlockStyle s;
  s.textAlignDefined = true;
  s.alignment = (paragraphAlignment == static_cast<uint8_t>(CssTextAlign::None))
                    ? CssTextAlign::Justify
                    : static_cast<CssTextAlign>(paragraphAlignment);
  return s;
}

BlockStyle MarkdownToBoxParser::headerStyle() const {
  BlockStyle s;
  s.textAlignDefined = true;
  s.alignment = CssTextAlign::Center;
  return s;
}

BlockStyle MarkdownToBoxParser::indentedStyle() const {
  BlockStyle s;
  s.textAlignDefined = true;
  s.alignment = CssTextAlign::Left;
  // Hanging/quote indent ~ one em from the left margin.
  const int16_t em = static_cast<int16_t>(renderer.getFontAscenderSize(fontId));
  s.marginLeft = std::max<int16_t>(1, em);
  return s;
}

// -------- block lifecycle --------
void MarkdownToBoxParser::ensureBlock() {
  if (!currentTextBlock) {
    currentTextBlock.reset(new ParsedText(extraParagraphSpacing, hyphenationEnabled, focusReadingEnabled,
                                          paragraphStyle()));
  }
}

void MarkdownToBoxParser::startNewBlock(const BlockStyle& style) {
  // Reuse an empty block; just retag its style (matches ChapterHtmlSlimParser::startNewTextBlock).
  if (currentTextBlock && currentTextBlock->isEmpty()) {
    currentTextBlock->setBlockStyle(style);
    return;
  }
  if (currentTextBlock) {
    flushBlock();
  }
  currentTextBlock.reset(new ParsedText(extraParagraphSpacing, hyphenationEnabled, focusReadingEnabled, style));
}

// flush partWordBuffer to the current block with the effective style.
void MarkdownToBoxParser::flushWord() {
  if (partWordBufferIndex <= 0) return;
  ensureBlock();
  partWordBuffer[partWordBufferIndex] = '\0';
  currentTextBlock->addWord(partWordBuffer, currentStyle());
  partWordBufferIndex = 0;
}

void MarkdownToBoxParser::accumulateText(const char* text, const uint16_t length) {
  for (uint16_t i = 0; i < length; i++) {
    const char c = text[i];
    if (isWhitespaceChar(c)) {
      flushWord();
      continue;
    }
    // Auto-break overlong words at a UTF-8 safe boundary (mirror ChapterHtmlSlimParser).
    if (partWordBufferIndex >= kMaxWordSize) {
      const int safeLen = utf8SafeTruncateBuffer(partWordBuffer, partWordBufferIndex);
      if (safeLen < partWordBufferIndex && safeLen > 0) {
        const int overflow = partWordBufferIndex - safeLen;
        char saved[4];
        for (int j = 0; j < overflow; j++) saved[j] = partWordBuffer[safeLen + j];
        partWordBufferIndex = safeLen;
        flushWord();
        for (int j = 0; j < overflow; j++) partWordBuffer[j] = saved[j];
        partWordBufferIndex = overflow;
      } else {
        flushWord();
      }
    }
    partWordBuffer[partWordBufferIndex++] = c;
  }
}

void MarkdownToBoxParser::flushBlock() {
  flushWord();
  if (!currentTextBlock || currentTextBlock->isEmpty()) {
    currentTextBlock.reset();
    return;
  }

  if (!currentPage) {
    currentPage.reset(new Page());
    currentPageNextY = 0;
  }

  const int lineHeight = static_cast<int>(renderer.getLineHeight(fontId) * lineCompression);
  const BlockStyle blockStyle = currentTextBlock->getBlockStyle();  // copy: survives reset below

  // top spacing
  currentPageNextY = static_cast<int16_t>(currentPageNextY + blockStyle.topInset());

  const int horizontalInset = blockStyle.totalHorizontalInset();
  const uint16_t effectiveWidth =
      (horizontalInset < viewportWidth) ? static_cast<uint16_t>(viewportWidth - horizontalInset) : viewportWidth;

  currentTextBlock->layoutAndExtractLines(
      renderer, fontId, effectiveWidth,
      [this](const std::shared_ptr<TextBlock>& line) { addLineToPage(line); });

  // bottom spacing
  currentPageNextY = static_cast<int16_t>(currentPageNextY + blockStyle.bottomInset());
  if (extraParagraphSpacing) {
    currentPageNextY = static_cast<int16_t>(currentPageNextY + lineHeight / 2);
  }

  currentTextBlock.reset();
}

void MarkdownToBoxParser::addLineToPage(const std::shared_ptr<TextBlock>& line) {
  const int lineHeight = static_cast<int>(renderer.getLineHeight(fontId) * lineCompression);

  if (!currentPage) {
    currentPage.reset(new Page());
    currentPageNextY = 0;
  }

  if (currentPageNextY + lineHeight > viewportHeight) {
    completePageFn(std::move(currentPage), paragraphIndex, listItemIndex);
    completedPageCount++;
    currentPage.reset(new Page());
    currentPageNextY = 0;
    // Low-heap early-stop check at a page boundary (a safe place to bail).
    checkLowHeapStop();
  }

  // Attach any pending link footnotes to the page the link lands on.
  if (!pendingFootnotes_.empty()) {
    for (const auto& [label, href] : pendingFootnotes_) {
      currentPage->addFootnote(label.c_str(), href.c_str());
    }
    pendingFootnotes_.clear();
  }

  const int16_t xOffset = line->getBlockStyle().leftInset();
  currentPage->elements.push_back(std::make_shared<PageLine>(line, xOffset, currentPageNextY));
  currentPageNextY = static_cast<int16_t>(currentPageNextY + lineHeight);
}

// Mirror ChapterHtmlSlimParser::emitHorizontalRule (simplified: default spacing, no CSS).
void MarkdownToBoxParser::emitHorizontalRule() {
  flushWord();
  if (currentTextBlock && !currentTextBlock->isEmpty()) {
    flushBlock();
  }
  if (!currentPage) {
    currentPage.reset(new Page());
    currentPageNextY = 0;
  }

  const int16_t lineHeight = static_cast<int16_t>(renderer.getLineHeight(fontId) * lineCompression + 0.5f);
  const int16_t spacing = static_cast<int16_t>(lineHeight / 2);
  constexpr uint8_t thickness = 2;
  const int16_t width = std::max<int16_t>(1, static_cast<int16_t>(viewportWidth / 4));
  const int16_t xPos = static_cast<int16_t>((viewportWidth - width) / 2);
  const int16_t total = static_cast<int16_t>(spacing + thickness + spacing);

  if (!currentPage->elements.empty() && currentPageNextY + total > viewportHeight) {
    completePageFn(std::move(currentPage), paragraphIndex, listItemIndex);
    completedPageCount++;
    currentPage.reset(new Page());
    currentPageNextY = 0;
  }

  currentPageNextY = static_cast<int16_t>(currentPageNextY + spacing);
  currentPage->elements.push_back(
      std::shared_ptr<PageHorizontalRule>(new PageHorizontalRule(width, thickness, xPos, currentPageNextY)));
  currentPageNextY = static_cast<int16_t>(currentPageNextY + thickness + spacing);
}

bool MarkdownToBoxParser::checkLowHeapStop() {
  if (heap_caps_get_free_size(MALLOC_CAP_8BIT) < kLowHeapEarlyStop) {
    LOG_ERR(TAG, "Low-heap early-stop: free=%zu < %zu", heap_caps_get_free_size(MALLOC_CAP_8BIT), kLowHeapEarlyStop);
    hitMemoryLimit_ = true;
    return true;
  }
  return false;
}

// Block-split tripwire: at a safe (newline) boundary, if a paragraph has grown large,
// flush it once and continue the SAME style in a fresh block. Bounds the transient peak
// regardless of paragraph length (ROUTE-A-MEMORY.md / papyrix Issue #137).
void MarkdownToBoxParser::maybeSplitBlock() {
  if (!currentTextBlock || currentTextBlock->size() <= kBlockSplitWordCheck) return;
  const bool atWordCap = currentTextBlock->size() >= kMaxWordsPerBlock;
  if (atWordCap || heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < kBlockSplitMinFreeBlock) {
    const BlockStyle style = currentTextBlock->getBlockStyle();
    flushBlock();
    if (!hitMemoryLimit_) {
      startNewBlock(style);
    }
  }
}

// -------- token dispatch --------
bool MarkdownToBoxParser::tokenTrampoline(const md_token_t* token, void* userData) {
  return static_cast<MarkdownToBoxParser*>(userData)->onToken(token);
}

bool MarkdownToBoxParser::onToken(const md_token_t* token) {
  if (hitMemoryLimit_ || aborted_) return false;  // returning false halts md_parse_chunk

  switch (token->type) {
    case MD_TEXT:
      if (inLinkText_) {
        linkLabel_.append(token->text, token->length);
      }
      if (inHeader_) {
        // Capture the heading's plain text for the free "#"/"##" TOC anchor (consumed at
        // MD_HEADER_END). Without this, headerText_ stays empty and no anchor is ever recorded.
        headerText_.append(token->text, token->length);
      }
      accumulateText(token->text, token->length);
      sawTextSinceNewline_ = true;
      break;

    case MD_HEADER_START:
      flushBlock();
      if (hitMemoryLimit_) return false;
      headerAnchorPage_ = completedPageCount;  // page where this header will begin
      headerLevel_ = (token->data >= 1 && token->data <= 6) ? static_cast<uint8_t>(token->data) : 1;
      headerText_.clear();
      startNewBlock(headerStyle());
      inHeader_ = true;
      break;

    case MD_HEADER_END:
      flushWord();
      if (!headerText_.empty()) {
        anchorData.push_back({headerText_, headerAnchorPage_, headerLevel_});  // free "#"/"##" TOC anchor
      }
      inHeader_ = false;
      flushBlock();
      if (hitMemoryLimit_) return false;
      startNewBlock(paragraphStyle());
      sawTextSinceNewline_ = false;
      break;

    case MD_BOLD_START:
      flushWord();
      inBold_ = true;
      break;
    case MD_BOLD_END:
      flushWord();
      inBold_ = false;
      break;

    case MD_ITALIC_START:
      flushWord();
      inItalic_ = true;
      break;
    case MD_ITALIC_END:
      flushWord();
      inItalic_ = false;
      break;

    case MD_STRIKE_START:
      flushWord();
      inStrike_ = true;
      break;
    case MD_STRIKE_END:
      flushWord();
      inStrike_ = false;
      break;

    case MD_CODE_INLINE:
      flushWord();
      inInlineCode_ = true;
      accumulateText(token->text, token->length);
      flushWord();
      inInlineCode_ = false;
      sawTextSinceNewline_ = true;
      break;

    case MD_CODE_BLOCK_START:
      flushBlock();
      if (hitMemoryLimit_) return false;
      startNewBlock(indentedStyle());
      inCodeBlock_ = true;
      break;
    case MD_CODE_BLOCK_END:
      flushBlock();
      if (hitMemoryLimit_) return false;
      inCodeBlock_ = false;
      startNewBlock(paragraphStyle());
      sawTextSinceNewline_ = false;
      break;

    case MD_LIST_ITEM_START: {
      flushBlock();
      if (hitMemoryLimit_) return false;
      listItemIndex++;
      startNewBlock(indentedStyle());
      ensureBlock();
      if (token->data > 0) {
        char numBuf[8];
        snprintf(numBuf, sizeof(numBuf), "%u.", static_cast<unsigned>(token->data));
        currentTextBlock->addWord(numBuf, EpdFontFamily::REGULAR);
      } else {
        currentTextBlock->addWord(BULLET, EpdFontFamily::REGULAR);
      }
      break;
    }

    case MD_BLOCKQUOTE_START:
      flushBlock();
      if (hitMemoryLimit_) return false;
      startNewBlock(indentedStyle());
      inBlockquote_ = true;
      break;
    case MD_BLOCKQUOTE_END:
      flushBlock();
      if (hitMemoryLimit_) return false;
      inBlockquote_ = false;
      startNewBlock(paragraphStyle());
      sawTextSinceNewline_ = false;
      break;

    case MD_HR:
      emitHorizontalRule();
      break;

    case MD_LINK_TEXT_START:
      flushWord();
      inLink_ = true;
      inLinkText_ = true;
      linkLabel_.clear();
      break;
    case MD_LINK_TEXT_END:
      flushWord();
      inLinkText_ = false;
      inLink_ = false;
      break;
    case MD_LINK_URL:
      // text + URL via Page::addFootnote (attached when the line lands on a page).
      pendingFootnotes_.push_back({linkLabel_.empty() ? std::string("link") : linkLabel_,
                                   std::string(token->text, token->length)});
      break;

    case MD_IMAGE_ALT_START:
      flushWord();
      inImageAlt_ = true;
      ensureBlock();
      currentTextBlock->addWord("[Image:", currentStyle());
      sawTextSinceNewline_ = true;
      break;
    case MD_IMAGE_ALT_END:
      flushWord();
      ensureBlock();
      currentTextBlock->addWord("]", currentStyle());
      inImageAlt_ = false;
      break;
    case MD_IMAGE_URL:
      break;  // placeholder only for now (SPEC s4.2)

    case MD_NEWLINE:
      flushWord();
      // Periodic external abort check (every kAbortCheckEveryLines lines).
      if (shouldAbort && (++lineCounter_ % kAbortCheckEveryLines == 0) && shouldAbort()) {
        aborted_ = true;
        return false;
      }
      if (inCodeBlock_) {
        // Render code blocks line-by-line (no wrapping/joining across source lines).
        flushBlock();
        if (hitMemoryLimit_) return false;
        startNewBlock(indentedStyle());
      } else if (!sawTextSinceNewline_) {
        // Second consecutive newline => blank line => paragraph break.
        if (currentTextBlock && !currentTextBlock->isEmpty()) {
          flushBlock();
          if (hitMemoryLimit_) return false;
          startNewBlock(paragraphStyle());
        }
      } else {
        // Single soft line break inside a paragraph: keep filling the same block.
        maybeSplitBlock();
        if (hitMemoryLimit_) return false;
      }
      sawTextSinceNewline_ = false;
      break;

    case MD_PARAGRAPH_START:
    case MD_PARAGRAPH_END:
    case MD_LIST_ITEM_END:
      break;  // not emitted by this tokenizer
  }

  return true;
}

// -------- file driver --------
// Reads in kReadChunkSize storage chunks and feeds md_parse_chunk on NEWLINE-ALIGNED
// boundaries: md_parser's text spans are pointers into the caller buffer, and a span is
// only guaranteed flushed at a '\n'. Ending every feed at a newline therefore prevents a
// span pointer from dangling across a reused buffer. The partial last line is carried to
// the front of the next read.
bool MarkdownToBoxParser::parseFile(const std::string& path) {
  HalFile file;
  if (!Storage.openFileForRead(TAG, path, file)) {
    LOG_ERR(TAG, "Failed to open %s", path.c_str());
    return false;
  }

  const size_t cap = kReadChunkSize;
  auto buf = std::unique_ptr<char[]>(new (std::nothrow) char[cap + 1]);
  if (!buf) {
    LOG_ERR(TAG, "Failed to allocate %zu B read buffer", cap + 1);
    file.close();
    return false;
  }

  md_parser_t parser;
  md_parser_init(&parser, tokenTrampoline, this);

  startNewBlock(paragraphStyle());

  size_t carry = 0;
  bool ok = true;
  while (!hitMemoryLimit_ && !aborted_) {
    const int n = file.read(reinterpret_cast<uint8_t*>(buf.get()) + carry, static_cast<size_t>(cap - carry));
    const size_t got = (n > 0) ? static_cast<size_t>(n) : 0;
    const size_t total = carry + got;
    const bool eof = (got == 0) || (file.available() == 0);

    if (total == 0) break;

    // Find the feed boundary: last '\n' in the buffer (whole buffer at EOF).
    size_t feedLen;
    if (eof) {
      feedLen = total;
    } else {
      size_t nl = total;
      while (nl > 0 && buf[nl - 1] != '\n') nl--;
      feedLen = (nl > 0) ? nl : total;  // no newline in a full buffer => overlong line; feed all
    }

    if (md_parse_chunk(&parser, buf.get(), feedLen) < 0) {
      ok = false;
      break;
    }

    if (feedLen < total) {
      carry = total - feedLen;
      memmove(buf.get(), buf.get() + feedLen, carry);
    } else {
      carry = 0;
      // Pathological >chunk line with no newline: the parser may hold a span pointing
      // into buf, which we are about to overwrite. Force a flush with a synthetic newline
      // so no dangling pointer survives (fail-safe; real notes have newlines < 8 KB).
      if (!eof && feedLen > 0 && buf[feedLen - 1] != '\n') {
        const char nlc = '\n';
        md_parse_chunk(&parser, &nlc, 1);
      }
    }

    if (eof) break;
  }

  md_parse_end(&parser);
  file.close();

  // Finalize the trailing block + page.
  if (ok && !hitMemoryLimit_) {
    flushBlock();
  } else {
    flushWord();  // best-effort: keep whatever fit
  }
  if (currentPage && !currentPage->elements.empty()) {
    completePageFn(std::move(currentPage), paragraphIndex, listItemIndex);
    completedPageCount++;
  }
  currentPage.reset();
  currentTextBlock.reset();

  LOG_INF(TAG, "Parsed %u pages (truncated=%d, aborted=%d)", completedPageCount, hitMemoryLimit_, aborted_);
  return ok;
}
