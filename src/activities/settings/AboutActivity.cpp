#include "AboutActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/QrUtils.h"

#ifndef CROSSPOINT_VERSION
#define CROSSPOINT_VERSION "unknown"
#endif

namespace {
constexpr char kSupportUrl[] = "https://cw1.me/discord";
}  // namespace

void AboutActivity::onEnter() {
  Activity::onEnter();
  requestUpdate();
}

void AboutActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_ABOUT));

  int y = metrics.topPadding + metrics.headerHeight + 36;
  renderer.drawCenteredText(UI_12_FONT_ID, y, "PAPERBIT", true, EpdFontFamily::BOLD);
  y += 30;
  renderer.drawCenteredText(SMALL_FONT_ID, y, CROSSPOINT_VERSION);
  y += 46;

  renderer.drawCenteredText(UI_10_FONT_ID, y, "Parts derived from CrossPoint", true);
  y += 30;
  renderer.drawCenteredText(UI_10_FONT_ID, y, "Updated by Chipp Walters,", true);
  y += 26;
  renderer.drawCenteredText(UI_10_FONT_ID, y, "Altuit, Inc.", true);
  y += 50;

  renderer.drawCenteredText(UI_10_FONT_ID, y, "Scan for support", true);
  y += 18;

  constexpr int qrSize = 220;
  QrUtils::drawQrCode(renderer, Rect{(pageWidth - qrSize) / 2, y, qrSize, qrSize}, kSupportUrl);
  y += qrSize + 26;

  renderer.drawCenteredText(SMALL_FONT_ID, y, kSupportUrl);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

void AboutActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
      mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    finish();
  }
}
