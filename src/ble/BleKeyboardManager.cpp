// BleKeyboardManager implementation — see BleKeyboardManager.h for scope/attribution and the
// three HARD DESIGN RULES (power lock, scan-stop-before-connect, address-by-value) found on
// hardware during the §6 gate runs.
//
// Entire translation unit is gated on ENABLE_BLE_KEYBOARD: in the default / slim builds this
// compiles to an empty object so the normal firmware is unaffected and NimBLE is never linked.

#ifdef ENABLE_BLE_KEYBOARD

#include "BleKeyboardManager.h"

#include <WiFi.h>
#include <esp_wifi.h>

#include <NimBLEDevice.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstring>

#include "Logging.h"

// --- Standard HID keyboard GATT UUIDs (16-bit) ---
static const NimBLEUUID HID_SERVICE_UUID((uint16_t)0x1812);
static const NimBLEUUID REPORT_UUID((uint16_t)0x2A4D);           // Report (Input/Output/Feature)
static const NimBLEUUID PROTOCOL_MODE_UUID((uint16_t)0x2A4E);    // Protocol Mode (0=Boot, 1=Report)
static const NimBLEUUID BOOT_KB_INPUT_UUID((uint16_t)0x2A22);    // Boot Keyboard Input Report
static const NimBLEUUID REPORT_REF_UUID((uint16_t)0x2908);       // Report Reference descriptor

// Per-attempt connect timeout. Bounds the worst-case join in deinit(): an in-flight attempt
// finishes within this before the abort flag is honored between retries.
#ifndef BLEKB_CONNECT_TIMEOUT_MS
#define BLEKB_CONNECT_TIMEOUT_MS 8000
#endif

// Worker task for the async connect/scan flows. NimBLE client calls block the calling task on
// host-task semaphores; 5 KB of stack is comfortably above what the gate flows used.
static constexpr uint32_t BLEKB_TASK_STACK = 5120;

// HID boot-keyboard modifier masks (byte 0).
static constexpr uint8_t MOD_SHIFT_LEFT = 0x02;
static constexpr uint8_t MOD_SHIFT_RIGHT = 0x20;
static inline bool isShiftMod(uint8_t m) { return (m & MOD_SHIFT_LEFT) || (m & MOD_SHIFT_RIGHT); }

// HID usage id for ErrorRollOver (all six key slots = 0x01 when too many keys are held).
static constexpr uint8_t HID_ERR_ROLLOVER = 0x01;

// ---------------------------------------------------------------------------------------------
// Client security callbacks: "Just Works" auto-accept. Mirrors MicroSlate's approach so keyboards
// that demand a passkey/confirm still bond without a UI (the keyboard being paired IS the input
// device, so there is nothing to type a code into on our side).
// ---------------------------------------------------------------------------------------------
static class BleKbClientCallbacks : public NimBLEClientCallbacks {
  void onConnect(NimBLEClient*) override { LOG_INF("BLEKB", "link connected"); }
  void onDisconnect(NimBLEClient*, int reason) override {
    LOG_INF("BLEKB", "link disconnected reason=%d", reason);
    BleKeyboardManager::getInstance().notifyDisconnected();
  }
  // Reject the keyboard's conn-param update (some keyboards send one on first keypress and crash
  // NimBLE 2.x if we echo our params back) — keep the negotiated interval. Same fix as MicroSlate.
  bool onConnParamsUpdateRequest(NimBLEClient*, const ble_gap_upd_params*) override { return false; }
  void onPassKeyEntry(NimBLEConnInfo& connInfo) override {
    LOG_INF("BLEKB", "passkey entry -> injecting 123456");
    NimBLEDevice::injectPassKey(connInfo, 123456);
  }
  void onConfirmPasskey(NimBLEConnInfo& connInfo, uint32_t pin) override {
    LOG_INF("BLEKB", "confirm passkey %06lu -> accept", (unsigned long)pin);
    NimBLEDevice::injectConfirmPasskey(connInfo, true);
  }
  void onAuthenticationComplete(NimBLEConnInfo& connInfo) override {
    LOG_INF("BLEKB", "auth complete encrypted=%d bonded=%d", connInfo.isEncrypted(),
            connInfo.isBonded());
  }
} kbClientCallbacks;

// Early-abort scan callback: stop scanning the moment a HID keyboard is spotted. This (a) lets the
// blocking getResults() return in ~1-2 s instead of the full timeout (keyboards drop out of pairing
// mode quickly), and (b) ensures the scanner is STOPPED before we connect — NimBLE rejects
// connect-while-scanning with EBUSY (observed 2026-07-01: found at rssi=-30, connect failed in 1 ms).
static class EarlyAbortScanCallbacks : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* dev) override {
    if (dev && dev->isAdvertisingService(NimBLEUUID((uint16_t)0x1812))) {
      NimBLEDevice::getScan()->stop();
    }
  }
} earlyAbortScanCallbacks;

BleKeyboardManager& BleKeyboardManager::getInstance() {
  static BleKeyboardManager instance;
  return instance;
}

void BleKeyboardManager::notifyDisconnected() {
  // Only a CONNECTED → dropped transition is a "disconnect"; failures during connect keep FAILED.
  State expected = State::CONNECTED;
  state_.compare_exchange_strong(expected, State::DISCONNECTED, std::memory_order_acq_rel);
}

bool BleKeyboardManager::begin() {
  if (initialized_) return true;

  // Radio discipline: BLE and Wi-Fi are mutually exclusive on the C3's single radio. Stop Wi-Fi
  // fully before bringing up the BLE controller (mirrors main.cpp / KOReaderSyncActivity).
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
  }
  esp_wifi_stop();  // idempotent; ensures the controller is released even if Arduino state lags

  if (!NimBLEDevice::init("PaperBit")) {
    LOG_ERR("BLEKB", "NimBLEDevice::init failed");
    state_.store(State::FAILED, std::memory_order_release);
    return false;
  }

  // Central / HID-host security: bond=true, MITM=false, SC=false, IO=NoInputNoOutput => "Just
  // Works". ENC-only key distribution (omit IRK) — matches MicroSlate's verified-compatible set.
  NimBLEDevice::setSecurityAuth(true, false, false);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
  NimBLEDevice::setSecurityInitKey(BLE_SM_PAIR_KEY_DIST_ENC);
  NimBLEDevice::setSecurityRespKey(BLE_SM_PAIR_KEY_DIST_ENC);
  NimBLEDevice::setPower(-9);  // lowest verified working TX power (MicroSlate)

  initialized_ = true;
  state_.store(State::IDLE, std::memory_order_release);
  LOG_INF("BLEKB", "NimBLE central initialized");
  return true;
}

// Shared connect body. peerAddr MUST be a value copy (never a pointer into scan results — see
// design rule 3). Right after a scan stops the controller can still be transitioning and
// ble_gap_connect rejects instantly, hence settle + retry.
bool BleKeyboardManager::connectPeer(const NimBLEAddress& peerAddr, int attempts) {
  state_.store(State::CONNECTING, std::memory_order_release);
  client_ = NimBLEDevice::createClient();
  if (!client_) {
    LOG_ERR("BLEKB", "createClient failed (connection pool exhausted?)");
    state_.store(State::FAILED, std::memory_order_release);
    return false;
  }
  client_->setClientCallbacks(&kbClientCallbacks, false);
  client_->setConnectTimeout(BLEKB_CONNECT_TIMEOUT_MS);

  bool connected = false;
  for (int attempt = 1; attempt <= attempts && !connected; ++attempt) {
    if (abort_.load(std::memory_order_acquire)) break;
    delay(attempt == 1 ? 150 : 350);  // settle before first try, back off on retries
    connected = client_->connect(peerAddr);
    if (!connected) {
      LOG_INF("BLEKB", "connect attempt %d failed (rc=%d), retrying...", attempt,
              client_->getLastError());
    }
  }
  if (!connected) {
    LOG_ERR("BLEKB", "connect failed to %s after retries", targetAddr_.c_str());
    NimBLEDevice::deleteClient(client_);
    client_ = nullptr;
    state_.store(State::FAILED, std::memory_order_release);
    return false;
  }

  // Best-effort pairing/encryption. Some keyboards need it, some don't — proceed to HID either
  // way. With bond=true this also creates/refreshes the NVS bond (CONFIG_BT_NIMBLE_NVS_PERSIST=1
  // in NimBLE-Arduino, so bonds survive deinit and reboot as long as deinit(clearBonds=false)).
  if (client_->secureConnection()) {
    LOG_INF("BLEKB", "secureConnection ok");
  } else {
    LOG_INF("BLEKB", "secureConnection not established — trying HID anyway");
  }

  if (!setupHidConnection()) {
    LOG_ERR("BLEKB", "HID setup/subscribe failed");
    if (client_->isConnected()) client_->disconnect();
    state_.store(State::FAILED, std::memory_order_release);
    return false;
  }

  memset(prevKeys_, 0, sizeof(prevKeys_));
  state_.store(State::CONNECTED, std::memory_order_release);
  LOG_INF("BLEKB", "keyboard connected + subscribed");
  return true;
}

bool BleKeyboardManager::scanAndConnect(uint32_t timeoutMs) {
  if (!initialized_ && !begin()) return false;

  // --- Blocking scan for a device advertising the HID service (0x1812) ---
  state_.store(State::SCANNING, std::memory_order_release);
  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setScanCallbacks(&earlyAbortScanCallbacks, false);  // early-abort when a HID keyboard appears
  scan->setActiveScan(true);
  scan->setInterval(1349);
  scan->setWindow(449);
  LOG_INF("BLEKB", "scanning for HID keyboard (%lu ms)...", (unsigned long)timeoutMs);

  NimBLEScanResults results = scan->getResults(timeoutMs, false);
  scan->stop();  // must be fully stopped before connect (EBUSY otherwise)

  // IMPORTANT: the advertised-device objects are OWNED by the scan and freed by clearResults().
  // Copy the peer address BY VALUE before clearing — connecting via the stale pointer is
  // use-after-free (observed on hardware: first-ever run connected by luck, every later run got
  // garbage → ble_gap_connect rc=3 BLE_HS_EINVAL).
  bool found = false;
  NimBLEAddress peerAddr;
  std::string peerName;
  for (int i = 0; i < results.getCount(); ++i) {
    const NimBLEAdvertisedDevice* dev = results.getDevice(i);
    if (dev && dev->isAdvertisingService(HID_SERVICE_UUID)) {
      found = true;
      peerAddr = dev->getAddress();  // value copy — safe past clearResults()
      peerName = dev->getName();     // value copy too
      LOG_INF("BLEKB", "found HID device: %s rssi=%d", peerAddr.toString().c_str(),
              dev->getRSSI());
      break;
    }
  }
  scan->clearResults();

  if (!found) {
    // Fail loud: Classic-only / USB-dongle keyboards never advertise 0x1812. Empty result is a
    // real, reportable condition, not a silent no-op.
    LOG_ERR("BLEKB", "no BLE HID keyboard found (0x1812) — is it in pairing mode & BLE (not Classic)?");
    state_.store(State::FAILED, std::memory_order_release);
    return false;
  }

  targetAddr_ = peerAddr.toString();
  targetAddrType_ = peerAddr.getType();
  connectedName_ = peerName;

  return connectPeer(peerAddr, 3);
}

bool BleKeyboardManager::connectToAddress(const std::string& addr, uint8_t addrType,
                                          const std::string& displayName) {
  if (!initialized_ && !begin()) return false;
  if (addr.empty()) return false;

  targetAddr_ = addr;
  targetAddrType_ = addrType;
  connectedName_ = displayName;
  LOG_INF("BLEKB", "direct connect to bonded keyboard %s (type=%u)", addr.c_str(), addrType);

  const NimBLEAddress peerAddr(addr, addrType);
  return connectPeer(peerAddr, 2);
}

std::vector<BleKeyboardManager::FoundKeyboard> BleKeyboardManager::scanForKeyboards(uint32_t scanMs) {
  std::vector<FoundKeyboard> out;
  if (!initialized_ && !begin()) return out;

  state_.store(State::SCANNING, std::memory_order_release);
  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setScanCallbacks(nullptr, false);  // picker scan: run the FULL window, collect everything
  scan->setActiveScan(true);               // active scan → we get scan-response names
  scan->setInterval(1349);
  scan->setWindow(449);
  LOG_INF("BLEKB", "picker scan for HID keyboards (%lu ms)...", (unsigned long)scanMs);

  NimBLEScanResults results = scan->getResults(scanMs, false);
  scan->stop();

  for (int i = 0; i < results.getCount(); ++i) {
    const NimBLEAdvertisedDevice* dev = results.getDevice(i);
    if (!dev || !dev->isAdvertisingService(HID_SERVICE_UUID)) continue;
    // All fields copied BY VALUE — the device objects die with clearResults().
    FoundKeyboard kb;
    kb.addr = dev->getAddress().toString();
    kb.addrType = dev->getAddress().getType();
    kb.name = dev->getName();
    kb.rssi = dev->getRSSI();
    // Dedupe by address, keep strongest RSSI / first non-empty name.
    bool merged = false;
    for (auto& existing : out) {
      if (existing.addr == kb.addr) {
        if (kb.rssi > existing.rssi) existing.rssi = kb.rssi;
        if (existing.name.empty() && !kb.name.empty()) existing.name = kb.name;
        merged = true;
        break;
      }
    }
    if (!merged) out.push_back(std::move(kb));
  }
  scan->clearResults();

  LOG_INF("BLEKB", "picker scan done: %u HID device(s)", (unsigned)out.size());
  state_.store(State::IDLE, std::memory_order_release);
  return out;
}

// --------------------------------------------------------------------------------------------
// Async wrappers: one short-lived worker task so activity loops stay responsive during the
// 1-3 s connect / multi-second scan. The task only touches the singleton; results are published
// through busy_ (release) and read by the UI after isBusy() returns false (acquire).
// --------------------------------------------------------------------------------------------

void BleKeyboardManager::asyncTaskTrampoline(void* param) {
  auto* self = static_cast<BleKeyboardManager*>(param);
  self->asyncTaskBody();
  self->busy_.store(false, std::memory_order_release);
  vTaskDelete(nullptr);
}

void BleKeyboardManager::asyncTaskBody() {
  switch (asyncOp_) {
    case AsyncOp::CONNECT: {
      if (!begin()) return;  // state already FAILED
      bool ok = false;
      if (!pendingAddr_.empty()) {
        ok = connectToAddress(pendingAddr_, pendingAddrType_, pendingName_);
      }
      if (!ok && !abort_.load(std::memory_order_acquire) &&
          (pendingAddr_.empty() || pendingScanFallback_)) {
        if (!pendingAddr_.empty()) {
          LOG_INF("BLEKB", "direct connect failed — falling back to scan");
        }
        scanAndConnect(12000);
      }
      break;
    }
    case AsyncOp::SCAN: {
      if (!begin()) return;
      scanResults_ = scanForKeyboards(pendingScanMs_);
      break;
    }
    case AsyncOp::NONE:
      break;
  }
}

bool BleKeyboardManager::startConnectAsync(const std::string& addr, uint8_t addrType,
                                           const std::string& displayName, bool scanFallback) {
  bool expected = false;
  if (!busy_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
    LOG_ERR("BLEKB", "startConnectAsync: worker already running");
    return false;
  }
  abort_.store(false, std::memory_order_release);
  asyncOp_ = AsyncOp::CONNECT;
  pendingAddr_ = addr;
  pendingAddrType_ = addrType;
  pendingName_ = displayName;
  pendingScanFallback_ = scanFallback;
  state_.store(State::CONNECTING, std::memory_order_release);
  if (xTaskCreate(&asyncTaskTrampoline, "BleKbWork", BLEKB_TASK_STACK, this, 1, nullptr) != pdPASS) {
    LOG_ERR("BLEKB", "startConnectAsync: task create failed");
    busy_.store(false, std::memory_order_release);
    state_.store(State::FAILED, std::memory_order_release);
    return false;
  }
  return true;
}

bool BleKeyboardManager::startScanAsync(uint32_t scanMs) {
  bool expected = false;
  if (!busy_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
    LOG_ERR("BLEKB", "startScanAsync: worker already running");
    return false;
  }
  abort_.store(false, std::memory_order_release);
  asyncOp_ = AsyncOp::SCAN;
  pendingScanMs_ = scanMs;
  scanResults_.clear();
  state_.store(State::SCANNING, std::memory_order_release);
  if (xTaskCreate(&asyncTaskTrampoline, "BleKbWork", BLEKB_TASK_STACK, this, 1, nullptr) != pdPASS) {
    LOG_ERR("BLEKB", "startScanAsync: task create failed");
    busy_.store(false, std::memory_order_release);
    state_.store(State::FAILED, std::memory_order_release);
    return false;
  }
  return true;
}

void BleKeyboardManager::requestAbort() {
  abort_.store(true, std::memory_order_release);
  // Stopping the scanner from another task is safe and makes a blocking getResults() return
  // early. A connect attempt in flight ends within BLEKB_CONNECT_TIMEOUT_MS; the abort flag is
  // then honored between retries.
  if (initialized_) {
    NimBLEScan* scan = NimBLEDevice::getScan();
    if (scan && scan->isScanning()) scan->stop();
  }
}

bool BleKeyboardManager::forgetAllBonds() {
  const bool wasInitialized = initialized_;
  if (!initialized_ && !begin()) return false;
  NimBLEDevice::deleteAllBonds();
  LOG_INF("BLEKB", "all NimBLE bonds deleted");
  if (!wasInitialized) {
    // We only brought the stack up for the wipe — tear it straight back down.
    deinit(false);
  }
  return true;
}

bool BleKeyboardManager::setupHidConnection() {
  if (!client_ || !client_->isConnected()) return false;

  if (client_->getServices(true).empty()) {
    LOG_ERR("BLEKB", "service discovery failed");
    return false;
  }
  hidService_ = client_->getService(HID_SERVICE_UUID);
  if (!hidService_) {
    LOG_ERR("BLEKB", "HID service (0x1812) not present");
    return false;
  }

  // Request Boot Protocol (write 0x00 to Protocol Mode 0x2A4E). Boot protocol gives a fixed 8-byte
  // report layout (mod, reserved, 6 keycodes) so we can decode without parsing the HID report map.
  NimBLERemoteCharacteristic* proto = hidService_->getCharacteristic(PROTOCOL_MODE_UUID);
  if (proto) {
    const uint8_t bootMode = 0x00;
    proto->writeValue(&bootMode, 1, true);
    LOG_INF("BLEKB", "Protocol Mode <- Boot (0x00)");
  } else {
    LOG_INF("BLEKB", "no Protocol Mode char — keyboard likely boot-only");
  }

  // Notify sink -> singleton decode. Runs on the NimBLE host task.
  auto onNotify = [](NimBLERemoteCharacteristic*, uint8_t* data, size_t len, bool) {
    BleKeyboardManager::getInstance().handleReport(data, len);
  };

  // Preferred path: the input Report characteristic (0x2A4D) whose Report Reference descriptor
  // (0x2908) marks it Input (type byte == 1). This is the report-protocol input report; subscribing
  // to it works on virtually all keyboards (many keep sending report-mode notifications even after a
  // Boot-Protocol request). Capture its report-ID prefix, if any.
  inputChar_ = nullptr;
  inputReportId_ = 0;
  const auto& chars = hidService_->getCharacteristics(true);
  for (auto* chr : chars) {
    if (chr->getUUID() != REPORT_UUID) continue;
    for (auto* d : chr->getDescriptors()) {
      if (d->getUUID() != REPORT_REF_UUID) continue;
      NimBLEAttValue ref = d->readValue();
      if (ref.size() >= 2 && (uint8_t)ref[1] == 1) {  // type 1 = Input
        inputChar_ = chr;
        inputReportId_ = (uint8_t)ref[0];
        LOG_INF("BLEKB", "selected input report (reportId=%u)", inputReportId_);
        break;
      }
    }
    if (inputChar_) break;
  }

  // Fallback 1: the dedicated Boot Keyboard Input Report (0x2A22) — the correct source in true
  // boot protocol mode.
  if (!inputChar_) {
    inputChar_ = hidService_->getCharacteristic(BOOT_KB_INPUT_UUID);
    if (inputChar_) {
      inputReportId_ = 0;
      LOG_INF("BLEKB", "using Boot Keyboard Input Report (0x2A22)");
    }
  }

  // Fallback 2: any notifiable 0x2A4D report char.
  if (!inputChar_) {
    for (auto* chr : chars) {
      if (chr->getUUID() == REPORT_UUID && chr->canNotify()) {
        inputChar_ = chr;
        inputReportId_ = 0;
        LOG_INF("BLEKB", "fallback: first notifiable report char");
        break;
      }
    }
  }

  if (!inputChar_) {
    LOG_ERR("BLEKB", "no keyboard input report characteristic found");
    return false;
  }

  if (!inputChar_->subscribe(true, onNotify)) {
    LOG_ERR("BLEKB", "subscribe to input report failed");
    return false;
  }
  LOG_INF("BLEKB", "subscribed to input report");
  return true;
}

// Boot-keyboard report -> emit one char per newly-pressed key. Ported from MicroSlate
// input_handler.cpp (diff-against-previous + hidToAscii).
void BleKeyboardManager::handleReport(const uint8_t* data, size_t len) {
  if (!data || len == 0) return;

  // Strip a leading report-ID byte if this keyboard uses one.
  if (inputReportId_ != 0 && data[0] == inputReportId_) {
    ++data;
    --len;
  }
  if (len < 7 || len > 8) return;  // not a keyboard report (media/consumer control etc.)

  // Normalize to 8-byte: [mod][reserved][k0..k5].
  uint8_t rep[8] = {0};
  if (len == 8) {
    memcpy(rep, data, 8);
  } else {  // 7-byte compact: insert reserved byte
    rep[0] = data[0];
    memcpy(&rep[2], &data[1], 6);
  }

  const uint8_t mods = rep[0];
  const uint8_t* keys = &rep[2];  // 6 slots

  // Ignore the all-rollover phantom (every slot == 0x01).
  bool allRollover = true;
  for (int i = 0; i < 6; ++i) {
    if (keys[i] != HID_ERR_ROLLOVER) { allRollover = false; break; }
  }
  if (allRollover) return;

  // Emit for keys present now but absent in the previous report (key-down edge).
  for (int i = 0; i < 6; ++i) {
    const uint8_t k = keys[i];
    if (k == 0 || k == HID_ERR_ROLLOVER) continue;
    bool wasDown = false;
    for (int j = 0; j < 6; ++j) {
      if (prevKeys_[j] == k) { wasDown = true; break; }
    }
    if (wasDown) continue;

    // CapsLock is a local toggle, not a printable.
    if (k == 0x39) {  // HID CapsLock
      capsLock_ = !capsLock_;
      continue;
    }
    const char c = hidToChar(k, mods);
    if (c != 0 && onChar_) onChar_(c);
  }

  memcpy(prevKeys_, keys, 6);
}

// HID usage -> ASCII. Ported from MicroSlate input_handler.cpp::hidToAscii. Shift table matches
// KeyboardEntryActivity's symbol semantics (US layout). Returns 0 for non-text usages.
char BleKeyboardManager::hidToChar(uint8_t hid, uint8_t modifiers) const {
  const bool shift = isShiftMod(modifiers);
  const bool letterShift = shift ^ capsLock_;

  // Letters a-z (0x04..0x1D)
  if (hid >= 0x04 && hid <= 0x1D) {
    const char base = 'a' + (hid - 0x04);
    return letterShift ? (char)(base - 32) : base;
  }
  // Number row (0x1E..0x27)
  if (hid >= 0x1E && hid <= 0x27) {
    static const char lo[] = "1234567890";
    static const char hi[] = "!@#$%^&*()";
    const int idx = hid - 0x1E;
    return shift ? hi[idx] : lo[idx];
  }
  switch (hid) {
    case 0x28: return '\n';   // Enter
    case 0x2A: return 0x08;   // Backspace -> BS (caller/editor interprets)
    case 0x2B: return '\t';   // Tab
    case 0x2C: return ' ';    // Space
    case 0x2D: return shift ? '_' : '-';
    case 0x2E: return shift ? '+' : '=';
    case 0x2F: return shift ? '{' : '[';
    case 0x30: return shift ? '}' : ']';
    case 0x31: return shift ? '|' : '\\';
    case 0x33: return shift ? ':' : ';';
    case 0x34: return shift ? '"' : '\'';
    case 0x35: return shift ? '~' : '`';
    case 0x36: return shift ? '<' : ',';
    case 0x37: return shift ? '>' : '.';
    case 0x38: return shift ? '?' : '/';
    default: return 0;
  }
}

bool BleKeyboardManager::isConnected() const {
  return state_.load(std::memory_order_acquire) == State::CONNECTED && client_ && client_->isConnected();
}

void BleKeyboardManager::deinit(bool clearBonds) {
  // Join any running worker first — tearing NimBLE down under a task blocked inside a client
  // call would crash. Bounded: abort stops the scanner instantly and a connect attempt ends
  // within BLEKB_CONNECT_TIMEOUT_MS.
  if (busy_.load(std::memory_order_acquire)) {
    requestAbort();
    const unsigned long t0 = millis();
    while (busy_.load(std::memory_order_acquire) && millis() - t0 < BLEKB_CONNECT_TIMEOUT_MS * 2 + 4000) {
      delay(20);
    }
    if (busy_.load(std::memory_order_acquire)) {
      // Fail loud and refuse the teardown rather than crash the worker mid-call. The stack stays
      // up (heap stays allocated) — visible in logs and the next begin() is a no-op.
      LOG_ERR("BLEKB", "deinit: worker did not stop — SKIPPING teardown (stack left up)");
      return;
    }
  }

  if (client_) {
    if (client_->isConnected()) client_->disconnect();
    NimBLEDevice::deleteClient(client_);
    client_ = nullptr;
  }
  inputChar_ = nullptr;
  hidService_ = nullptr;

  if (initialized_) {
    if (clearBonds) NimBLEDevice::deleteAllBonds();
    NimBLEDevice::deinit(true);  // free the controller heap — critical for the C3 RAM budget
    initialized_ = false;
  }
  state_.store(State::IDLE, std::memory_order_release);
  memset(prevKeys_, 0, sizeof(prevKeys_));
  connectedName_.clear();
  LOG_INF("BLEKB", "deinit complete (bonds %s)", clearBonds ? "cleared" : "kept");
}

#endif  // ENABLE_BLE_KEYBOARD
