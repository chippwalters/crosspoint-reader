#pragma once
#include <string>

// Single configurable source URL for "Paperbit Fetch", persisted as plain text at
// /.crosspoint/fetch.url. It can be set three ways, all of which just write that file:
//   - on-device (the on-screen URL keyboard in PaperbitFetchActivity)
//   - the Wi-Fi web settings page
//   - the desktop (Paperbit) client over USB
// The device fetches <url>/index.json to list documents and <url>/<name>.epub to download
// them (see paperbit/docs and the convert.php prototype). Plain text keeps every setter trivial.
class FetchSourceStore {
 public:
  static FetchSourceStore& getInstance() { return instance; }

  // Returns the configured base URL (empty string if unset). Loads from SD on first use.
  const std::string& getUrl();

  // Trim, persist to /.crosspoint/fetch.url, and update the cache. Returns false on write error.
  bool setUrl(const std::string& url);

 private:
  static FetchSourceStore instance;
  FetchSourceStore() = default;

  bool loaded = false;
  std::string url;
  void ensureLoaded();
};

#define FETCH_SOURCE FetchSourceStore::getInstance()
