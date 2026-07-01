#include "PaperbitFetchActivity.h"

#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include <cstdio>

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
const char* kSetUrlRow = "Set source URL";  // ASCII only (UI font may lack pencil/ellipsis glyphs)

// Case-insensitive suffix test (ASCII).
bool endsWithI(const std::string& s, const std::string& suf) {
  if (s.size() < suf.size()) return false;
  const size_t off = s.size() - suf.size();
  for (size_t i = 0; i < suf.size(); i++) {
    char a = s[off + i], b = suf[i];
    if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
    if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
    if (a != b) return false;
  }
  return true;
}

// A fetchable document type (what the reader can open). Excludes index.json / .css / .html
// directory-listing chrome by construction.
bool isSupportedDoc(const std::string& f) {
  return endsWithI(f, ".md") || endsWithI(f, ".epub") || endsWithI(f, ".txt") ||
         endsWithI(f, ".xtc") || endsWithI(f, ".xtch");
}
}  // namespace

void PaperbitFetchActivity::onEnter() {
  Activity::onEnter();
  // Re-read the source URL from disk so a value set from the desktop client (over USB) or the
  // Wi-Fi web page is picked up without a reboot (the store otherwise caches the boot-time value).
  FETCH_SOURCE.reload();
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
    errorHint = "Couldn't join Wi-Fi. Try again.";
    requestUpdate();
  }
}

void PaperbitFetchActivity::fetchIndex() {
  errorHint.clear();
  docs.clear();

  // 1. OPTIONAL index.json — if the folder has one (curated display names / order), use it.
  //    Absent index.json is the normal "just drop files" case, not an error.
  const std::string indexUrl = UrlUtils::buildUrl(baseUrl, "index.json");
  LOG_DBG("FETCH", "Index (optional): %s", indexUrl.c_str());
  std::string body;
  int status = 0;
  if (HttpDownloader::fetchUrl(indexUrl, body, status) && status == 200) {
    JsonDocument doc;
    if (!deserializeJson(doc, body) && doc.is<JsonArray>()) {
      for (JsonObject o : doc.as<JsonArray>()) {
        FetchDoc d;
        d.name = o["name"] | "";
        d.file = o["file"] | "";
        if (d.file.empty() && !d.name.empty()) d.file = d.name + ".md";  // name-only shorthand
        if (!d.file.empty()) docs.push_back(d);
      }
    }
  }

  // 2. No usable index.json — list the folder itself and take every supported file (drop-and-fetch).
  if (docs.empty()) {
    LOG_DBG("FETCH", "Listing folder: %s", baseUrl.c_str());
    std::string listing;
    int listStatus = 0;
    if (!HttpDownloader::fetchUrl(baseUrl, listing, listStatus)) {
      state = State::ERROR;
      if (listStatus == 0) {
        // Connection never opened: on Wi-Fi but no internet/DNS, or a bad host.
        errorMessage = "Can't reach the source";
        errorHint = "Check your Wi-Fi and the source URL.";
      } else if (listStatus == 404) {
        errorMessage = "Source not found (404)";
        errorHint = "Check the source folder URL.";
      } else {
        char buf[40];
        snprintf(buf, sizeof(buf), "Source error (HTTP %d)", listStatus);
        errorMessage = buf;
        errorHint = "Unexpected server response.";
      }
      requestUpdate();
      return;
    }
    parseDirectoryListing(listing);
  }

  if (docs.empty()) {
    // Reached the source but found no documents (empty folder, or listing disabled + no index.json).
    state = State::ERROR;
    errorMessage = "No documents found";
    errorHint = "Add .md / .epub files to the source folder.";
    requestUpdate();
    return;
  }

  selectorIndex = 1;  // land on the first doc
  state = State::LIST;
  requestUpdate();
}

// Parse an Apache autoindex (directory-listing) HTML page: collect every href that is a plain
// filename ending in a supported document extension. This is what makes "drop a file in the
// folder and it shows up" work with no index.json. Skips the parent-dir, column-sort (?C=...),
// absolute-path, and subdirectory links, and de-dupes (a listing links each name twice: icon + text).
void PaperbitFetchActivity::parseDirectoryListing(const std::string& html) {
  const std::string marker = "href=\"";
  size_t pos = 0;
  while ((pos = html.find(marker, pos)) != std::string::npos) {
    pos += marker.size();
    const size_t end = html.find('"', pos);
    if (end == std::string::npos) break;
    const std::string href = html.substr(pos, end - pos);
    pos = end + 1;
    if (href.empty() || href[0] == '?' || href[0] == '/' || href.find('/') != std::string::npos ||
        href.find("..") != std::string::npos) {
      continue;
    }
    if (!isSupportedDoc(href)) continue;
    bool dup = false;
    for (const auto& d : docs) {
      if (d.file == href) { dup = true; break; }
    }
    if (dup) continue;
    FetchDoc d;
    d.file = href;
    d.name = href;  // show the filename with its extension (per product choice)
    docs.push_back(d);
  }
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
    // saveToFile() is REQUIRED — ESP.restart() clears RAM, and the reader-target boot path
    // reloads APP_STATE from disk, so the new path must be persisted before the reboot.
    APP_STATE.openEpubPath = dest;
    APP_STATE.saveToFile();
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
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 20, errorMessage.c_str(), true, EpdFontFamily::BOLD);
    if (!errorHint.empty()) {
      renderer.drawCenteredText(SMALL_FONT_ID, pageHeight / 2 + 12, errorHint.c_str(), true);
    }
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
