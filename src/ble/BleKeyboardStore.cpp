// BleKeyboardStore implementation — see header. Gated: empty TU without ENABLE_BLE_KEYBOARD.

#ifdef ENABLE_BLE_KEYBOARD

#include "BleKeyboardStore.h"

#include <HalStorage.h>
#include <Logging.h>

#include <cstdio>
#include <cstdlib>

namespace {
constexpr const char* kPath = "/.crosspoint/blekeyboard.cfg";

std::string trimmedLine(const std::string& s, size_t from, size_t to) {
  size_t b = from;
  while (b < to && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r')) ++b;
  size_t e = to;
  while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r')) --e;
  return s.substr(b, e - b);
}
}  // namespace

BleKeyboardStore& BleKeyboardStore::getInstance() {
  static BleKeyboardStore instance;
  return instance;
}

void BleKeyboardStore::ensureLoaded() {
  if (loaded_) return;
  loaded_ = true;
  addr_.clear();
  addrType_ = 0;
  name_.clear();
  if (!Storage.exists(kPath)) return;

  const String raw = Storage.readFile(kPath);
  const std::string s(raw.c_str());
  // Split into (up to) 3 lines: addr / type / name.
  size_t pos = 0;
  int lineNo = 0;
  while (pos <= s.size() && lineNo < 3) {
    size_t nl = s.find('\n', pos);
    if (nl == std::string::npos) nl = s.size();
    const std::string line = trimmedLine(s, pos, nl);
    switch (lineNo) {
      case 0: addr_ = line; break;
      case 1: addrType_ = static_cast<uint8_t>(strtoul(line.c_str(), nullptr, 10)); break;
      case 2: name_ = line; break;
    }
    pos = nl + 1;
    ++lineNo;
  }
  if (!addr_.empty()) {
    LOG_DBG("BLEKB", "stored keyboard: %s (type=%u, name='%s')", addr_.c_str(), addrType_, name_.c_str());
  }
}

bool BleKeyboardStore::hasKeyboard() {
  ensureLoaded();
  return !addr_.empty();
}

const std::string& BleKeyboardStore::getAddress() {
  ensureLoaded();
  return addr_;
}

uint8_t BleKeyboardStore::getAddressType() {
  ensureLoaded();
  return addrType_;
}

std::string BleKeyboardStore::getDisplayName() {
  ensureLoaded();
  return name_.empty() ? addr_ : name_;
}

bool BleKeyboardStore::save(const std::string& addr, uint8_t addrType, const std::string& name) {
  Storage.ensureDirectoryExists("/.crosspoint");
  char buf[160];
  snprintf(buf, sizeof(buf), "%s\n%u\n%s\n", addr.c_str(), addrType, name.c_str());
  if (!Storage.writeFile(kPath, String(buf))) {
    LOG_ERR("BLEKB", "failed to write %s", kPath);
    return false;
  }
  addr_ = addr;
  addrType_ = addrType;
  name_ = name;
  loaded_ = true;
  LOG_INF("BLEKB", "keyboard saved: %s (%s)", name.c_str(), addr.c_str());
  return true;
}

bool BleKeyboardStore::clear() {
  addr_.clear();
  addrType_ = 0;
  name_.clear();
  loaded_ = true;
  if (Storage.exists(kPath) && !Storage.remove(kPath)) {
    LOG_ERR("BLEKB", "failed to remove %s", kPath);
    return false;
  }
  LOG_INF("BLEKB", "stored keyboard cleared");
  return true;
}

#endif  // ENABLE_BLE_KEYBOARD
