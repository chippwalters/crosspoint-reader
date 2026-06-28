#include "VaultReaderActivity.h"

#include <GfxRenderer.h>

#include <algorithm>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "crypto/VaultCrypto.h"
#include "fontIds.h"

namespace {
constexpr int BODY_FONT = UI_12_FONT_ID;
}

VaultReaderActivity::~VaultReaderActivity() {
  if (!content.empty()) VaultCrypto::wipe(&content[0], content.size());
}

void VaultReaderActivity::onExit() {
  Activity::onExit();
  if (!content.empty()) VaultCrypto::wipe(&content[0], content.size());
  content.clear();
  lines.clear();
}

void VaultReaderActivity::wrapInto(const std::string& paragraph, int viewportWidth, int fontId) {
  if (paragraph.empty()) {
    lines.emplace_back();
    return;
  }
  std::string line = paragraph;
  while (!line.empty()) {
    if (renderer.getTextAdvanceX(fontId, line.c_str(), EpdFontFamily::REGULAR) <= viewportWidth) {
      lines.push_back(line);
      break;
    }
    size_t breakPos = line.length();
    while (breakPos > 0 &&
           renderer.getTextAdvanceX(fontId, line.substr(0, breakPos).c_str(), EpdFontFamily::REGULAR) > viewportWidth) {
      size_t spacePos = line.rfind(' ', breakPos - 1);
      if (spacePos != std::string::npos && spacePos > 0) {
        breakPos = spacePos;
      } else {
        breakPos--;
        while (breakPos > 0 && (static_cast<unsigned char>(line[breakPos]) & 0xC0) == 0x80) breakPos--;
      }
    }
    if (breakPos == 0) breakPos = 1;
    lines.push_back(line.substr(0, breakPos));
    size_t skip = breakPos;
    if (breakPos < line.length() && line[breakPos] == ' ') skip++;
    line = line.substr(skip);
  }
}

void VaultReaderActivity::initialize() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  marginLeft = metrics.contentSidePadding;
  marginTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;

  const int viewportWidth = renderer.getScreenWidth() - marginLeft * 2;
  const int footer = metrics.buttonHintsHeight + metrics.verticalSpacing;
  const int viewportHeight = renderer.getScreenHeight() - marginTop - footer;
  const int lineHeight = renderer.getLineHeight(BODY_FONT);
  linesPerPage = std::max(1, viewportHeight / lineHeight);

  // Wrap each source line (split on '\n') into display lines.
  size_t pos = 0;
  while (pos <= content.size()) {
    size_t nl = content.find('\n', pos);
    std::string src = content.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
    if (!src.empty() && src.back() == '\r') src.pop_back();
    wrapInto(src, viewportWidth, BODY_FONT);
    if (nl == std::string::npos) break;
    pos = nl + 1;
  }

  if (lines.empty()) lines.emplace_back();
  totalPages = std::max(1, (static_cast<int>(lines.size()) + linesPerPage - 1) / linesPerPage);
  initialized = true;
}

void VaultReaderActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  buttonNavigator.onNext([this] {
    if (currentPage < totalPages - 1) {
      currentPage++;
      requestUpdate();
    }
  });
  buttonNavigator.onPrevious([this] {
    if (currentPage > 0) {
      currentPage--;
      requestUpdate();
    }
  });
}

void VaultReaderActivity::render(RenderLock&&) {
  if (!initialized) initialize();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, title.c_str());

  const int lineHeight = renderer.getLineHeight(BODY_FONT);
  const int start = currentPage * linesPerPage;
  const int end = std::min(start + linesPerPage, static_cast<int>(lines.size()));
  int y = marginTop;
  for (int i = start; i < end; ++i) {
    if (!lines[i].empty()) renderer.drawText(BODY_FONT, marginLeft, y, lines[i].c_str());
    y += lineHeight;
  }

  std::string pageLabel = std::to_string(currentPage + 1) + " / " + std::to_string(totalPages);
  const auto labels = mappedInput.mapLabels("Back", pageLabel.c_str(), "Prev", "Next");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
