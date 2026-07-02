// BleKeyboardPairingActivity — Settings > Keyboard: scan / pair / forget a BLE HID keyboard.
//
// Mirrors WifiSelectionActivity's scan → list → select pattern, adapted for BLE HID:
//  - Scan collects ALL devices advertising the HID service (0x1812) during the window
//    (name + RSSI); an empty result shows an explicit "BLE-only keyboards supported" screen
//    (Classic-only / USB-dongle keyboards never appear in a 0x1812 scan — fail loud).
//  - Select → connect + bond ("Just Works"; NimBLE persists the bond keys in NVS) → the device
//    address/type/name is saved to /.crosspoint/blekeyboard.cfg (BleKeyboardStore) so the Notes
//    editor can reconnect directly without scanning.
//  - "Forget keyboard" clears the NimBLE bonds AND the stored address, behind a confirm dialog.
//
// Radio + lifecycle: a HalPowerManager::Lock is held for the whole visit (hardware rule: the C3
// BLE controller deadlocks below 80 MHz) and BleKeyboardManager::deinit(false) runs on exit —
// bonds are KEPT. Scans/connects run on the manager's worker task so BACK always works.
//
// Only compiled under ENABLE_BLE_KEYBOARD (empty TU otherwise).
#pragma once

#ifdef ENABLE_BLE_KEYBOARD

#include <HalPowerManager.h>

#include <memory>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "ble/BleKeyboardManager.h"
#include "util/ButtonNavigator.h"

class BleKeyboardPairingActivity final : public Activity {
 public:
  explicit BleKeyboardPairingActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("BleKeyboardPairing", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override;

 private:
  enum class UiState : uint8_t {
    INFO,        // paired/unpaired summary + actions (Scan / Forget)
    SCANNING,    // picker scan in flight
    DEVICE_LIST, // scan results (or the explicit empty-result screen)
    CONNECTING,  // bonding with the selected keyboard
    PAIRED_OK,   // bond + store save succeeded
    PAIR_FAILED, // connect/bond failed
    EXITING      // BACK pressed while the worker was busy; waiting for it to stop
  };

  void startScan();
  void connectToSelection();
  void confirmForget();

  ButtonNavigator buttonNavigator;
  std::unique_ptr<HalPowerManager::Lock> powerLock_;
  UiState state_ = UiState::INFO;
  std::vector<BleKeyboardManager::FoundKeyboard> results_;
  size_t selectedIndex_ = 0;   // DEVICE_LIST selection
  size_t infoIndex_ = 0;       // INFO action selection
  bool workerWasBusy_ = false;
  std::string pendingName_;    // display name of the keyboard being bonded
  std::string errorText_;
};

#endif  // ENABLE_BLE_KEYBOARD
