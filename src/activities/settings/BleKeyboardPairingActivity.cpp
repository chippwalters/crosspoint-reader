// BleKeyboardPairingActivity implementation — see header. Gated: empty TU without
// ENABLE_BLE_KEYBOARD.

#ifdef ENABLE_BLE_KEYBOARD

#include "BleKeyboardPairingActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include "MappedInputManager.h"
#include "activities/util/ConfirmationActivity.h"
#include "ble/BleKeyboardStore.h"
#include "components/UITheme.h"
#include "fontIds.h"

// Picker scan window. Keyboards in pairing mode advertise aggressively, so a few seconds is
// enough to collect names + RSSI; the user can rescan from the list screen.
#ifndef BLEKB_PICKER_SCAN_MS
#define BLEKB_PICKER_SCAN_MS 6000
#endif

namespace {
const char* rssiBars(int rssi) {
  if (rssi >= -50) return "||||";
  if (rssi >= -60) return " |||";
  if (rssi >= -70) return "  ||";
  return "   |";
}
}  // namespace

void BleKeyboardPairingActivity::onEnter() {
  Activity::onEnter();
  // HARD RULE: hold normal CPU speed for the entire BLE session (C3 controller deadlocks at
  // 10 MHz). Released in onExit after deinit.
  powerLock_ = std::make_unique<HalPowerManager::Lock>();

  results_.clear();
  selectedIndex_ = 0;
  infoIndex_ = 0;
  errorText_.clear();
  workerWasBusy_ = false;

  if (BLEKB_STORE.hasKeyboard()) {
    state_ = UiState::INFO;  // show "Paired: <name>" + actions; scan only on request
  } else {
    startScan();  // nothing paired: go straight to scanning (WifiSelection pattern)
  }
  requestUpdate();
}

void BleKeyboardPairingActivity::onExit() {
  Activity::onExit();
  auto& kb = BleKeyboardManager::getInstance();
  // Production rule: deinit(false) — NEVER clear bonds on a normal exit (deinit joins the
  // worker task first, so this is safe even mid-scan/connect).
  kb.deinit(false);
  powerLock_.reset();
}

bool BleKeyboardPairingActivity::preventAutoSleep() {
  // Don't sleep mid-scan/bond; INFO and list screens follow the normal timeout.
  return BleKeyboardManager::getInstance().isBusy();
}

void BleKeyboardPairingActivity::startScan() {
  auto& kb = BleKeyboardManager::getInstance();
  results_.clear();
  selectedIndex_ = 0;
  errorText_.clear();
  if (!kb.startScanAsync(BLEKB_PICKER_SCAN_MS)) {
    errorText_ = "BLE scan could not start";
    state_ = UiState::PAIR_FAILED;
    requestUpdate();
    return;
  }
  workerWasBusy_ = true;
  state_ = UiState::SCANNING;
  requestUpdate();
}

void BleKeyboardPairingActivity::connectToSelection() {
  if (selectedIndex_ >= results_.size()) return;
  const auto& dev = results_[selectedIndex_];
  pendingName_ = dev.name.empty() ? dev.addr : dev.name;
  auto& kb = BleKeyboardManager::getInstance();
  if (!kb.startConnectAsync(dev.addr, dev.addrType, pendingName_, /*scanFallback=*/false)) {
    errorText_ = "Connect could not start";
    state_ = UiState::PAIR_FAILED;
    requestUpdate();
    return;
  }
  workerWasBusy_ = true;
  state_ = UiState::CONNECTING;
  requestUpdate();
}

void BleKeyboardPairingActivity::confirmForget() {
  startActivityForResult(
      std::make_unique<ConfirmationActivity>(renderer, mappedInput, "Forget keyboard?",
                                             BLEKB_STORE.getDisplayName()),
      [this](const ActivityResult& res) {
        if (!res.isCancelled) {
          auto& kb = BleKeyboardManager::getInstance();
          const bool bondsOk = kb.forgetAllBonds();
          const bool storeOk = BLEKB_STORE.clear();
          if (!bondsOk || !storeOk) {
            errorText_ = "Forget FAILED - see log";
            state_ = UiState::PAIR_FAILED;
          } else {
            state_ = UiState::INFO;
            infoIndex_ = 0;
          }
        }
        requestUpdate();
      });
}

void BleKeyboardPairingActivity::loop() {
  auto& kb = BleKeyboardManager::getInstance();
  const bool busy = kb.isBusy();

  // Worker-completion transitions
  if (workerWasBusy_ && !busy) {
    workerWasBusy_ = false;
    switch (state_) {
      case UiState::SCANNING:
        results_ = kb.getScanResults();
        selectedIndex_ = 0;
        state_ = UiState::DEVICE_LIST;
        requestUpdate();
        break;
      case UiState::CONNECTING:
        if (kb.getState() == BleKeyboardManager::State::CONNECTED) {
          const auto& dev = results_[selectedIndex_];
          if (BLEKB_STORE.save(dev.addr, dev.addrType, dev.name)) {
            state_ = UiState::PAIRED_OK;
          } else {
            errorText_ = "Bonded, but saving to SD FAILED";
            state_ = UiState::PAIR_FAILED;
          }
        } else {
          errorText_ = "Pairing failed - put the keyboard in pairing mode and retry";
          state_ = UiState::PAIR_FAILED;
        }
        requestUpdate();
        break;
      case UiState::EXITING:
        finish();
        return;
      default:
        break;
    }
  }

  // BACK: abort a busy worker first (scan stops instantly; a connect attempt ends within its
  // bounded timeout), then leave. Never blocks the loop.
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (busy) {
      kb.requestAbort();
      state_ = UiState::EXITING;
      requestUpdate();
    } else if (state_ == UiState::DEVICE_LIST || state_ == UiState::PAIRED_OK ||
               state_ == UiState::PAIR_FAILED) {
      if (BLEKB_STORE.hasKeyboard()) {
        state_ = UiState::INFO;
        infoIndex_ = 0;
        requestUpdate();
      } else {
        finish();
      }
    } else {
      finish();
    }
    return;
  }

  switch (state_) {
    case UiState::INFO: {
      const int actionCount = BLEKB_STORE.hasKeyboard() ? 2 : 1;  // Scan / (Forget)
      buttonNavigator.onNextRelease([this, actionCount] {
        infoIndex_ = ButtonNavigator::nextIndex(static_cast<int>(infoIndex_), actionCount);
        requestUpdate();
      });
      buttonNavigator.onPreviousRelease([this, actionCount] {
        infoIndex_ = ButtonNavigator::previousIndex(static_cast<int>(infoIndex_), actionCount);
        requestUpdate();
      });
      if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
        if (infoIndex_ == 0) {
          startScan();
        } else {
          confirmForget();
        }
      }
      break;
    }
    case UiState::DEVICE_LIST: {
      if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
        if (results_.empty()) {
          startScan();  // OK on the empty screen = rescan
        } else {
          connectToSelection();
        }
        return;
      }
      if (mappedInput.wasReleased(MappedInputManager::Button::Right) && !results_.empty()) {
        startScan();  // explicit rescan
        return;
      }
      const int count = static_cast<int>(results_.size());
      if (count > 0) {
        buttonNavigator.onNextRelease([this, count] {
          selectedIndex_ = ButtonNavigator::nextIndex(static_cast<int>(selectedIndex_), count);
          requestUpdate();
        });
        buttonNavigator.onPreviousRelease([this, count] {
          selectedIndex_ = ButtonNavigator::previousIndex(static_cast<int>(selectedIndex_), count);
          requestUpdate();
        });
      }
      break;
    }
    case UiState::PAIRED_OK:
    case UiState::PAIR_FAILED: {
      if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
        if (state_ == UiState::PAIRED_OK) {
          state_ = UiState::INFO;
          infoIndex_ = 0;
          requestUpdate();
        } else {
          startScan();  // FAILED: OK = retry scan
        }
      }
      break;
    }
    default:
      break;  // SCANNING / CONNECTING / EXITING: waiting on the worker
  }
}

void BleKeyboardPairingActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "Keyboard");

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;
  const int centerY = contentTop + contentHeight / 2;

  const char* backLbl = tr(STR_BACK);
  const char* okLbl = "";
  const char* upLbl = "";
  const char* downLbl = "";

  switch (state_) {
    case UiState::INFO: {
      const bool paired = BLEKB_STORE.hasKeyboard();
      const std::string status =
          paired ? ("Paired: " + BLEKB_STORE.getDisplayName()) : std::string("No keyboard paired");
      renderer.drawCenteredText(UI_12_FONT_ID, contentTop + 20, status.c_str(), true,
                                paired ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);

      std::vector<std::string> actions = {"Scan for keyboard"};
      if (paired) actions.emplace_back("Forget keyboard");
      GUI.drawList(
          renderer, Rect{0, contentTop + 70, pageWidth, contentHeight - 70}, static_cast<int>(actions.size()),
          static_cast<int>(infoIndex_), [&actions](int i) { return actions[i]; });
      okLbl = tr(STR_SELECT);
      upLbl = tr(STR_DIR_UP);
      downLbl = tr(STR_DIR_DOWN);
      break;
    }
    case UiState::SCANNING:
      renderer.drawCenteredText(UI_12_FONT_ID, centerY - 20, "Scanning for keyboards...", true, EpdFontFamily::BOLD);
      renderer.drawCenteredText(SMALL_FONT_ID, centerY + 20, "Put the keyboard in pairing mode");
      break;
    case UiState::DEVICE_LIST: {
      if (results_.empty()) {
        // Fail loud: this is the "your keyboard is not BLE" screen, never a silent empty list.
        renderer.drawCenteredText(UI_12_FONT_ID, centerY - 50, "No BLE keyboards found", true, EpdFontFamily::BOLD);
        renderer.drawCenteredText(SMALL_FONT_ID, centerY - 10, "Only Bluetooth LE (BLE) keyboards are supported.");
        renderer.drawCenteredText(SMALL_FONT_ID, centerY + 15, "Classic-BT / USB-dongle keyboards will not appear.");
        renderer.drawCenteredText(SMALL_FONT_ID, centerY + 55, "Press OK to scan again");
        okLbl = tr(STR_RETRY);
      } else {
        char countStr[32];
        snprintf(countStr, sizeof(countStr), "%u found - select to pair", (unsigned)results_.size());
        renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding, contentTop, countStr);
        GUI.drawList(
            renderer, Rect{0, contentTop + 30, pageWidth, contentHeight - 30}, static_cast<int>(results_.size()),
            static_cast<int>(selectedIndex_),
            [this](int i) { return results_[i].name.empty() ? results_[i].addr : results_[i].name; },
            nullptr, nullptr, [this](int i) { return std::string(rssiBars(results_[i].rssi)); });
        okLbl = "Pair";
        upLbl = tr(STR_DIR_UP);
        downLbl = tr(STR_DIR_DOWN);
      }
      break;
    }
    case UiState::CONNECTING:
      renderer.drawCenteredText(UI_12_FONT_ID, centerY - 20, "Pairing...", true, EpdFontFamily::BOLD);
      renderer.drawCenteredText(SMALL_FONT_ID, centerY + 20, pendingName_.c_str());
      break;
    case UiState::PAIRED_OK: {
      renderer.drawCenteredText(UI_12_FONT_ID, centerY - 20, "Keyboard paired!", true, EpdFontFamily::BOLD);
      const std::string who = "Paired: " + BLEKB_STORE.getDisplayName();
      renderer.drawCenteredText(UI_10_FONT_ID, centerY + 20, who.c_str());
      okLbl = tr(STR_DONE);
      break;
    }
    case UiState::PAIR_FAILED:
      renderer.drawCenteredText(UI_12_FONT_ID, centerY - 20, "Pairing FAILED", true, EpdFontFamily::BOLD);
      renderer.drawCenteredText(SMALL_FONT_ID, centerY + 20, errorText_.c_str());
      okLbl = tr(STR_RETRY);
      break;
    case UiState::EXITING:
      renderer.drawCenteredText(UI_10_FONT_ID, centerY, "Cancelling...");
      break;
  }

  const auto labels = mappedInput.mapLabels(backLbl, okLbl, upLbl, downLbl);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}

#endif  // ENABLE_BLE_KEYBOARD
