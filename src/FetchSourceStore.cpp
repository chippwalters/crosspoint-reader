#include "FetchSourceStore.h"

#include <HalStorage.h>
#include <Logging.h>

FetchSourceStore FetchSourceStore::instance;

namespace {
constexpr const char* kPath = "/.crosspoint/fetch.url";

std::string trimmed(const std::string& s) {
  const size_t b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return "";
  const size_t e = s.find_last_not_of(" \t\r\n");
  return s.substr(b, e - b + 1);
}
}  // namespace

void FetchSourceStore::ensureLoaded() {
  if (loaded) return;
  loaded = true;
  if (Storage.exists(kPath)) {
    const String s = Storage.readFile(kPath);
    url = trimmed(std::string(s.c_str()));
  }
}

const std::string& FetchSourceStore::getUrl() {
  ensureLoaded();
  return url;
}

bool FetchSourceStore::setUrl(const std::string& newUrl) {
  const std::string u = trimmed(newUrl);
  Storage.ensureDirectoryExists("/.crosspoint");
  if (!Storage.writeFile(kPath, String(u.c_str()))) {
    LOG_ERR("FETCH", "Failed to write %s", kPath);
    return false;
  }
  url = u;
  loaded = true;
  LOG_DBG("FETCH", "Source URL set: %s", u.c_str());
  return true;
}
