// BleKeyboardManager — NimBLE BLE-central / HID-host lifecycle + boot-keyboard decode.
//
// Scope: PaperBit BLE-keyboard *text-entry* (see docs/BLE-KEYBOARD-PLAN.md). The ESP32-C3 has a
// single radio shared between Wi-Fi and BLE, so BLE is only ever brought up inside an explicit
// text-entry path (Notes editor / keyboard pairing screen) and torn down on exit — never in the
// reader loop. Wi-Fi is stopped before NimBLEDevice::init() and this owns deinit().
//
// HARD DESIGN RULES (found on hardware during the §6 gate, commits a38d0208 / 9f0f6de1):
//   1. The C3 BLE controller DEADLOCKS if inited below 80 MHz. Callers MUST hold a
//      HalPowerManager::Lock for the ENTIRE BLE session (begin() → deinit()).
//   2. The scanner must be fully stopped before connect() (EBUSY otherwise) — early-abort scan
//      callback + explicit stop() + settle/retry.
//   3. NEVER hold pointers into scan results past clearResults() — copy NimBLEAddress BY VALUE.
//
// Ported/adapted (MIT, © Josh-writes) from github.com/Josh-writes/microslate-firmware
//   src/ble_keyboard.cpp  (scan/connect/HID discovery/subscribe)
//   src/input_handler.cpp (hidToAscii boot-keyboard decode + prevKeys diff)
//
// The whole class only exists when ENABLE_BLE_KEYBOARD is defined (excluded from the default and
// slim builds so the normal firmware is byte-for-byte unaffected).
#pragma once

#ifdef ENABLE_BLE_KEYBOARD

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// Forward declarations keep NimBLE headers out of the public interface (and out of the default
// build's include graph). The concrete types are only pulled in by BleKeyboardManager.cpp.
class NimBLEClient;
class NimBLERemoteService;
class NimBLERemoteCharacteristic;

class BleKeyboardManager {
 public:
  // DISCONNECTED = link dropped after a successful CONNECTED (peer power-off / range) — lets the
  // editor show a fail-loud status instead of silently dead keys.
  enum class State : uint8_t { IDLE, SCANNING, CONNECTING, CONNECTED, FAILED, DISCONNECTED };

  // A HID (0x1812) device seen during a picker scan.
  struct FoundKeyboard {
    std::string name;  // advertised name; may be empty (picker shows the address then)
    std::string addr;
    uint8_t addrType = 0;
    int rssi = 0;
  };

  // Emitted for every decoded key-down (printable + \n/\t/backspace-as-0x08). Runs on the NimBLE
  // host task, not the main loop — keep the sink cheap / lock-free (e.g. xQueueSend, timeout 0).
  using CharCallback = std::function<void(char c)>;

  static BleKeyboardManager& getInstance();

  // Radio discipline: stops Wi-Fi, then NimBLEDevice::init() as a central. Idempotent. Returns
  // false only if the BLE controller refuses to come up. Cheap heap-wise until scan/connect.
  // PRECONDITION: caller holds HalPowerManager::Lock (10 MHz init deadlocks the controller).
  bool begin();

  // --- Synchronous primitives (block the calling task) ---

  // Scan for the first device advertising the HID service (0x1812), connect, request Boot
  // Protocol, subscribe to the keyboard input report. Returns true once subscribed.
  // (Kept for the TEMP CMD:BLEGATE harness; feature flows use the async variants below.)
  bool scanAndConnect(uint32_t timeoutMs = 15000);

  // Direct connect to a previously bonded keyboard by stored address (no scan). displayName is
  // shown by getConnectedName() (the store remembers it; a direct connect has no adv data).
  bool connectToAddress(const std::string& addr, uint8_t addrType, const std::string& displayName);

  // Collect ALL HID (0x1812) devices seen during a scan window (for the pairing picker).
  // Deduped by address, strongest RSSI kept. Blocking.
  std::vector<FoundKeyboard> scanForKeyboards(uint32_t scanMs);

  // --- Async wrappers (spawn a short-lived FreeRTOS task so the UI loop stays responsive and
  //     the device buttons — BACK in particular — keep working during the 1–3 s connect) ---

  // If addr is non-empty: connect by address first; on failure (and scanFallback) fall back to a
  // scan-and-connect. If addr is empty: scan-and-connect. State: CONNECTING → CONNECTED/FAILED.
  // Returns false if a task is already running or the task can't be created.
  bool startConnectAsync(const std::string& addr, uint8_t addrType, const std::string& displayName,
                         bool scanFallback);

  // Picker scan: state SCANNING while running; results via getScanResults() once !isBusy().
  bool startScanAsync(uint32_t scanMs);

  // True while an async task is running. Poll from the activity loop.
  bool isBusy() const { return busy_.load(std::memory_order_acquire); }

  // Ask a running async op to stop early (stops the scanner immediately; a connect attempt in
  // flight finishes its bounded timeout first). The task then ends with State::FAILED.
  void requestAbort();

  // Only valid when !isBusy() after a startScanAsync.
  const std::vector<FoundKeyboard>& getScanResults() const { return scanResults_; }

  bool isConnected() const;
  State getState() const { return state_.load(std::memory_order_acquire); }
  const std::string& getConnectedName() const { return connectedName_; }

  // Wipe all NimBLE bonds from NVS (used by "Forget keyboard"). Brings the stack up temporarily
  // if needed. PRECONDITION: HalPowerManager::Lock held, Wi-Fi acceptable to stop.
  bool forgetAllBonds();

  // Tears the stack fully down (NimBLEDevice::deinit(true) frees the controller heap). Joins any
  // running async task first (bounded by the connect timeout). clearBonds wipes NVS bonds — the
  // gate harness uses true; PRODUCTION PATHS MUST PASS false so the pairing survives.
  void deinit(bool clearBonds);

  void setCharCallback(CharCallback cb) { onChar_ = std::move(cb); }

  // Fed the raw HID input report by the NimBLE notify callback. Normalizes 7/8-byte + report-ID
  // framing, diffs against prevKeys_, and emits one char per newly-pressed key.
  void handleReport(const uint8_t* data, size_t len);

  // Internal: called by the client callbacks on link loss.
  void notifyDisconnected();

 private:
  BleKeyboardManager() = default;
  BleKeyboardManager(const BleKeyboardManager&) = delete;
  BleKeyboardManager& operator=(const BleKeyboardManager&) = delete;

  bool setupHidConnection();
  // Shared connect body: create client, settle+retry connect BY ADDRESS (value), secure, HID setup.
  bool connectPeer(const class NimBLEAddress& peerAddr, int attempts);
  char hidToChar(uint8_t hid, uint8_t modifiers) const;

  static void asyncTaskTrampoline(void* param);
  void asyncTaskBody();

  std::atomic<State> state_{State::IDLE};
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
  std::string connectedName_;

  // Async-task plumbing. Written by the main task before the worker starts; results published
  // with release/acquire via busy_.
  std::atomic<bool> busy_{false};
  std::atomic<bool> abort_{false};
  enum class AsyncOp : uint8_t { NONE, CONNECT, SCAN } asyncOp_ = AsyncOp::NONE;
  std::string pendingAddr_;
  uint8_t pendingAddrType_ = 0;
  std::string pendingName_;
  bool pendingScanFallback_ = true;
  uint32_t pendingScanMs_ = 6000;
  std::vector<FoundKeyboard> scanResults_;
};

#endif  // ENABLE_BLE_KEYBOARD
