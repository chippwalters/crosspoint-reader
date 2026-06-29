#include "PaperbitFetchActivity.h"

#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include "CrossPointState.h"
#include "FetchSourceStore.h"
#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"
#include "util/UrlUtils.h"

namespace {
constexpr int PAGE_ITEMS = 8;
constexpr const char* FETCH_DIR = "/Fetch";
const char* kSetUrlRow = "\xE2\x9C\x8E Set source URL\xE2\x80\xA6";  // "✎ Set source URL…"
}  // namespace

void PaperbitFetchActivity::onEnter() {
  Activity::onEnter();
  baseUrl = FETCH_SOURCE.getUrl();
  if (baseUrl.empty()) {
    state = State::NO_URL;
    requestUpdate();
    return;
  }
  state = State::CHECK_WIFI;
  statusMessage = tr(STR_LOADING);
  requestUpdate();
  checkAndConnectWifi();
}

void PaperbitFetchActivity::onExit() {
  Activity::onExit();
  docs.clear();
  // Tear down Wi-Fi the same way the OPDS browser does: a silent reboot avoids heap
  // fragmentation from bringing the radio up and down within a long-lived session.
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    silentRestart();
  }
}

void PaperbitFetchActivity::checkAndConnectWifi() {
  if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
    state = State::LOADING;
    requestUpdate();
    fetchIndex();
    return;
  }
  launchWifiSelection();
}

void PaperbitFetchActivity::launchWifiSelection() {
  state = State::WIFI_SELECTION;
  requestUpdate();
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void PaperbitFetchActivity::onWifiSelectionComplete(const bool connected) {
  if (connected) {
    state = State::LOADING;
    requestUpdate(true);
    fetchIndex();
  } else {
    state = State::ERROR;
    errorMessage = tr(STR_WIFI_CONN_FAILED);
    requestUpdate();
  }
}

void PaperbitFetchActivity::fetchIndex() {
  const std::string url = UrlUtils::buildUrl(baseUrl, "index.json");
  LOG_DBG("FETCH", "Index: %s", url.c_str());

  std::string body;
  if (!HttpDownloader::fetchUrl(url, body)) {
    state = State::ERROR;
    errorMessage = "Could not reach source";
    requestUpdate();
    return;
  }

  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, body);
  if (err || !doc.is<JsonArray>()) {
    state = State::ERROR;
    errorMessage = "Bad index.json";
    requestUpdate();
    return;
  }

  docs.clear();
  for (JsonObject o : doc.as<JsonArray>()) {
    FetchDoc d;
    d.name = o["name"] | "";
    d.file = o["file"] | "";
    if (d.file.empty() && !d.name.empty()) d.file = d.name + ".epub";
    if (!d.file.empty()) docs.push_back(d);
  }

  selectorIndex = docs.empty() ? 0 : 1;  // land on the first doc when there is one
  state = State::LIST;
  requestUpdate();
}

void PaperbitFetchActivity::downloadDoc(const FetchDoc& d) {
  state = State::DOWNLOADING;
  statusMessage = d.name.empty() ? d.file : d.name;
  downloadProgress = downloadTotal = 0;
  requestUpdate(true);

  Storage.ensureDirectoryExists(FETCH_DIR);
  const std::string dest = std::string(FETCH_DIR) + "/" + d.file;
  const std::string url = UrlUtils::buildUrl(baseUrl, d.file);
  LOG_DBG("FETCH", "Download: %s -> %s", url.c_str(), dest.c_str());

  const auto result = HttpDownloader::downloadToFile(url, dest, [this](const size_t got, const size_t total) {
    downloadProgress = got;
    downloadTotal = total;
    requestUpdate(true);
  });

  if (result == HttpDownloader::OK) {
    // Open the freshly-fetched doc: set it as the target and reboot into the reader
    // (the reboot also performs the Wi-Fi teardown that onExit would otherwise do).
    APP_STATE.openEpubPath = dest;
    silentRestartToReader();
  } else {
    state = State::ERROR;
    errorMessage = tr(STR_DOWNLOAD_FAILED);
    requestUpdate();
  }
}

void PaperbitFetchActivity::promptForUrl() {
  startActivityForResult(
      std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, "Source URL", FETCH_SOURCE.getUrl(), 200,
                                              InputType::Url),
      [this](const ActivityResult& result) {
        if (!result.isCancelled) {
          FETCH_SOURCE.setUrl(std::get<KeyboardResult>(result.data).text);
          baseUrl = FETCH_SOURCE.getUrl();
        }
        if (baseUrl.empty()) {
          state = State::NO_URL;
          requestUpdate();
        } else {
          state = State::CHECK_WIFI;
          requestUpdate();
          checkAndConnectWifi();
        }
      });
}

void PaperbitFetchActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "Paperbit Fetch");

  if (state == State::NO_URL) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 20, "No source URL set", true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(SMALL_FONT_ID, pageHeight / 2 + 10, "Press Set URL to enter your folder URL.", true);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "Set URL", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == State::CHECK_WIFI || state == State::LOADING) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_LOADING));
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == State::WIFI_SELECTION) {
    renderer.displayBuffer();
    return;
  }

  if (state == State::ERROR) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 10, errorMessage.c_str(), true);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_RETRY), "", "Set URL");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == State::DOWNLOADING) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 40, tr(STR_DOWNLOADING));
    const auto t = renderer.truncatedText(UI_10_FONT_ID, statusMessage.c_str(), pageWidth - 40);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 10, t.c_str());
    if (downloadTotal > 0) {
      GUI.drawProgressBar(renderer, Rect{50, pageHeight / 2 + 20, pageWidth - 100, 20}, downloadProgress, downloadTotal);
    }
    renderer.displayBuffer();
    return;
  }

  // LIST: row 0 is "Set source URL", rows 1.. are the documents.
  std::vector<std::string> rows;
  rows.reserve(docs.size() + 1);
  rows.emplace_back(kSetUrlRow);
  for (const auto& d : docs) rows.push_back(d.name.empty() ? d.file : d.name);

  GUI.drawButtonMenu(
      renderer,
      Rect{0, metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing, pageWidth,
           pageHeight - (metrics.topPadding + metrics.headerHeight + metrics.buttonHintsHeight + metrics.verticalSpacing)},
      static_cast<int>(rows.size()), selectorIndex, [&rows](int i) { return rows[i]; },
      [](int i) { return i == 0 ? Settings : Book; });

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

void PaperbitFetchActivity::loop() {
  if (state == State::WIFI_SELECTION || state == State::DOWNLOADING) return;

  if (state == State::CHECK_WIFI || state == State::LOADING) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) finish();
    return;
  }

  if (state == State::NO_URL) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      promptForUrl();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      finish();
    }
    return;
  }

  if (state == State::ERROR) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      state = State::CHECK_WIFI;
      requestUpdate();
      checkAndConnectWifi();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
      promptForUrl();  // btn4 = "Set URL"
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      finish();
    }
    return;
  }

  // LIST
  const int rowCount = static_cast<int>(docs.size()) + 1;
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (selectorIndex == 0) {
      promptForUrl();
    } else if (selectorIndex - 1 < static_cast<int>(docs.size())) {
      downloadDoc(docs[selectorIndex - 1]);
    }
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
  }

  buttonNavigator.onNextRelease([this, rowCount] {
    selectorIndex = ButtonNavigator::nextIndex(selectorIndex, rowCount);
    requestUpdate();
  });
  buttonNavigator.onPreviousRelease([this, rowCount] {
    selectorIndex = ButtonNavigator::previousIndex(selectorIndex, rowCount);
    requestUpdate();
  });
  buttonNavigator.onNextContinuous([this, rowCount] {
    selectorIndex = ButtonNavigator::nextPageIndex(selectorIndex, rowCount, PAGE_ITEMS);
    requestUpdate();
  });
  buttonNavigator.onPreviousContinuous([this, rowCount] {
    selectorIndex = ButtonNavigator::previousPageIndex(selectorIndex, rowCount, PAGE_ITEMS);
    requestUpdate();
  });
}
