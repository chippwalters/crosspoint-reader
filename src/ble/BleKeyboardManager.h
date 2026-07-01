// BleKeyboardManager — NimBLE BLE-central / HID-host lifecycle + boot-keyboard decode.
//
// Scope: PaperBit BLE-keyboard *text-entry* (see docs/BLE-KEYBOARD-PLAN.md). The ESP32-C3 has a
// single radio shared between Wi-Fi and BLE, so BLE is only ever brought up inside an explicit
// text-entry path and torn down on exit — never in the reader loop. Wi-Fi is stopped before
// NimBLEDevice::init() and this owns deinit().
//
// This is the PREP skeleton: NimBLE lifecycle + boot-keyboard report decode only. It intentionally
// does NOT wire a full BleTextEntryActivity yet — that is gated on the §6 RAM go/no-go measurement
// run via the TEMP `CMD:BLEGATE` harness in main.cpp.
//
// Ported/adapted (MIT, © Josh-writes) from github.com/Josh-writes/microslate-firmware
//   src/ble_keyboard.cpp  (scan/connect/HID discovery/subscribe)
//   src/input_handler.cpp (hidToAscii boot-keyboard decode + prevKeys diff)
//
// The whole class only exists when ENABLE_BLE_KEYBOARD is defined (excluded from the default and
// slim builds so the normal firmware is byte-for-byte unaffected).
#pragma once

#ifdef ENABLE_BLE_KEYBOARD

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

// Forward declarations keep NimBLE headers out of the public interface (and out of the default
// build's include graph). The concrete types are only pulled in by BleKeyboardManager.cpp.
class NimBLEClient;
class NimBLERemoteService;
class NimBLERemoteCharacteristic;

class BleKeyboardManager {
 public:
  enum class State : uint8_t { IDLE, SCANNING, CONNECTING, CONNECTED, FAILED };

  // Emitted for every decoded key-down (printable + \n/\t/backspace-as-0x08). Runs on the NimBLE
  // host task, not the main loop — keep the sink cheap / lock-free.
  using CharCallback = std::function<void(char c)>;

  static BleKeyboardManager& getInstance();

  // Radio discipline: stops Wi-Fi, then NimBLEDevice::init() as a central. Idempotent. Returns
  // false only if the BLE controller refuses to come up. Cheap heap-wise until scanAndConnect().
  bool begin();

  // Blocking (up to timeoutMs): scan for a device advertising the HID service (0x1812), connect,
  // request Boot Protocol (write 0x00 to Protocol Mode 0x2A4E), and subscribe to the keyboard
  // input report. Returns true once subscribed. Designed to succeed the moment a real BLE HID
  // keyboard is powered on and in range.
  bool scanAndConnect(uint32_t timeoutMs = 15000);

  bool isConnected() const;
  State getState() const { return state_; }

  // Tears the stack fully down (NimBLEDevice::deinit(true) frees the controller heap). When
  // clearBonds is true, wipes NVS bonds first. Always call on exit from the BLE path.
  void deinit(bool clearBonds);

  void setCharCallback(CharCallback cb) { onChar_ = std::move(cb); }

  // Fed the raw HID input report by the NimBLE notify callback. Normalizes 7/8-byte + report-ID
  // framing, diffs against prevKeys_, and emits one char per newly-pressed key.
  void handleReport(const uint8_t* data, size_t len);

 private:
  BleKeyboardManager() = default;
  BleKeyboardManager(const BleKeyboardManager&) = delete;
  BleKeyboardManager& operator=(const BleKeyboardManager&) = delete;

  bool setupHidConnection();
  char hidToChar(uint8_t hid, uint8_t modifiers) const;

  State state_ = State::IDLE;
  bool initialized_ = false;
  bool capsLock_ = false;

  CharCallback onChar_;

  // Boot-keyboard report diff state: bytes 2..7 of the previous normalized report.
  uint8_t prevKeys_[6] = {0};
  uint8_t inputReportId_ = 0;  // non-zero if the keyboard prefixes reports with a report-ID byte

  NimBLEClient* client_ = nullptr;
  NimBLERemoteService* hidService_ = nullptr;
  NimBLERemoteCharacteristic* inputChar_ = nullptr;

  std::string targetAddr_;
  uint8_t targetAddrType_ = 0;
};

#endif  // ENABLE_BLE_KEYBOARD
