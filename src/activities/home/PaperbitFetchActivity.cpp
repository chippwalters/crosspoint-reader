#include "PaperbitFetchActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "FetchSourceStore.h"
#include "MappedInputManager.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

void PaperbitFetchActivity::onEnter() {
  Activity::onEnter();
  requestUpdate();
}

void PaperbitFetchActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "Paperbit Fetch");

  const std::string& url = FETCH_SOURCE.getUrl();
  int y = pageHeight / 3;
  if (url.empty()) {
    renderer.drawCenteredText(UI_10_FONT_ID, y, "No source URL set", true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(SMALL_FONT_ID, y + 30, "Press Set URL to enter your folder URL.", true);
  } else {
    renderer.drawCenteredText(UI_10_FONT_ID, y, "Source URL", true, EpdFontFamily::BOLD);
    const int wrapW = pageWidth - 2 * metrics.contentSidePadding;
    auto lines = renderer.wrappedText(SMALL_FONT_ID, url.c_str(), wrapW, 4);
    int yy = y + 28;
    for (const auto& ln : lines) {
      renderer.drawCenteredText(SMALL_FONT_ID, yy, ln.c_str(), true);
      yy += 22;
    }
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "Set URL", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

void PaperbitFetchActivity::promptForUrl() {
  startActivityForResult(
      std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, "Source URL", FETCH_SOURCE.getUrl(), 200,
                                              InputType::Url),
      [this](const ActivityResult& result) {
        if (!result.isCancelled) {
          FETCH_SOURCE.setUrl(std::get<KeyboardResult>(result.data).text);
        }
        requestUpdate();
      });
}

void PaperbitFetchActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    promptForUrl();
    return;
  }
}
