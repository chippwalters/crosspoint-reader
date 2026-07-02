// BleKeyboardStore — persistence for the ONE bonded BLE keyboard (address + type + name).
//
// The NimBLE bond keys themselves live in NVS (CONFIG_BT_NIMBLE_NVS_PERSIST=1); this store only
// remembers WHICH device we bonded to so the Notes editor can reconnect directly by address
// (no scan) and Settings can show "Paired: <name>". Plain-text file on SD, one value per line
// (same trivial-store precedent as /.crosspoint/fetch.url):
//   line 1: peer address ("aa:bb:cc:dd:ee:ff")
//   line 2: address type (decimal, NimBLE BLE_ADDR_* value)
//   line 3: display name (may be empty)
//
// Only compiled under ENABLE_BLE_KEYBOARD (empty TU otherwise).
#pragma once

#ifdef ENABLE_BLE_KEYBOARD

#include <cstdint>
#include <string>

class BleKeyboardStore {
 public:
  static BleKeyboardStore& getInstance();

  // True if a keyboard is stored. Loads from SD on first use.
  bool hasKeyboard();

  const std::string& getAddress();
  uint8_t getAddressType();
  // Display name; falls back to the address when the keyboard advertised no name.
  std::string getDisplayName();

  // Persist a newly bonded keyboard. Returns false on SD write error (fail loud in caller).
  bool save(const std::string& addr, uint8_t addrType, const std::string& name);

  // Forget: remove the stored file + cache. (NimBLE bonds are cleared separately by the caller
  // via BleKeyboardManager::forgetAllBonds().)
  bool clear();

  // Force a re-read on next access (e.g. after external modification).
  void reload() { loaded_ = false; }

 private:
  BleKeyboardStore() = default;
  void ensureLoaded();

  bool loaded_ = false;
  std::string addr_;
  uint8_t addrType_ = 0;
  std::string name_;
};

#define BLEKB_STORE BleKeyboardStore::getInstance()

#endif  // ENABLE_BLE_KEYBOARD
