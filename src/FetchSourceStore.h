#pragma once
#include <string>

// Single configurable source URL for "Paperbit Fetch", persisted as plain text at
// /.crosspoint/fetch.url. It can be set three ways, all of which just write that file:
//   - on-device (the on-screen URL keyboard in PaperbitFetchActivity)
//   - the Wi-Fi web settings page
//   - the desktop (Paperbit) client over USB
// The device tries <url>/index.json to list documents (OPTIONAL) and falls back to the server's
// directory listing; it then downloads each doc (.md/.epub/.txt/.xtc/.xtch) to /Fetch/ and opens
// it. Plain-http URL by design (in-session TLS is heap-infeasible on the C3). Plain-text storage
// keeps every setter trivial. See paperbit/ARCHITECTURE.md §5.5.
class FetchSourceStore {
 public:
  static FetchSourceStore& getInstance() { return instance; }

  // Returns the configured base URL (empty string if unset). Loads from SD on first use.
  const std::string& getUrl();

  // Trim, persist to /.crosspoint/fetch.url, and update the cache. Returns false on write error.
  bool setUrl(const std::string& url);

  // Force a re-read of /.crosspoint/fetch.url on the next getUrl(). Call this before reading
  // the URL so a value written externally (desktop client over USB, or the Wi-Fi web page)
  // is picked up WITHOUT a device reboot — otherwise the once-loaded cache stays stale.
  void reload() { loaded = false; }

 private:
  static FetchSourceStore instance;
  FetchSourceStore() = default;

  bool loaded = false;
  std::string url;
  void ensureLoaded();
};

#define FETCH_SOURCE FetchSourceStore::getInstance()
