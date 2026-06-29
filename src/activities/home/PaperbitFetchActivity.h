#pragma once

#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// Paperbit Fetch: pull refreshable documents from a single configured web folder.
// Flow: ensure Wi-Fi -> fetch <url>/index.json -> list docs -> on select, download
// <url>/<file> into /Fetch and reboot into the reader (the reboot also tears down
// Wi-Fi cleanly, mirroring the OPDS browser). The source URL is also editable here.
class PaperbitFetchActivity final : public Activity {
 public:
  enum class State { NO_URL, CHECK_WIFI, WIFI_SELECTION, LOADING, LIST, DOWNLOADING, ERROR };

  explicit PaperbitFetchActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("PaperbitFetch", renderer, mappedInput), buttonNavigator() {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  struct FetchDoc {
    std::string name;
    std::string file;
  };

  ButtonNavigator buttonNavigator;
  State state = State::CHECK_WIFI;
  std::string baseUrl;
  std::vector<FetchDoc> docs;
  int selectorIndex = 0;  // 0 = "Set source URL" row; 1.. = docs[index-1]
  std::string statusMessage;
  std::string errorMessage;
  size_t downloadProgress = 0;
  size_t downloadTotal = 0;

  void checkAndConnectWifi();
  void launchWifiSelection();
  void onWifiSelectionComplete(bool connected);
  void fetchIndex();
  void downloadDoc(const FetchDoc& doc);
  void promptForUrl();
  bool preventAutoSleep() override { return true; }
};
