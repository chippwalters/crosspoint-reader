#include "VaultActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cctype>
#include <memory>
#include <vector>

#include "VaultImportActivity.h"
#include "MappedInputManager.h"
#include "VaultConfigStore.h"
#include "VaultReaderActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr char VAULT_DIR[] = "/Vault";
constexpr char PBV_EXT[] = ".pbv";
constexpr size_t MAX_VAULT_FILE = 96 * 1024;  // guard against decrypting absurd blobs
constexpr size_t MAX_NOTE_CHARS = 4000;       // on-device keyboard note cap

bool hasPbvExtension(const std::string& name) {
  return name.size() > 4 && name.compare(name.size() - 4, 4, PBV_EXT) == 0;
}

// Read an entire (binary) file into a vector. Empty vector on failure.
std::vector<uint8_t> readBinary(const std::string& path) {
  std::vector<uint8_t> out;
  HalFile f;
  if (!Storage.openFileForRead("VAULT", path, f)) return out;
  const size_t n = f.size();
  if (n == 0 || n > MAX_VAULT_FILE) {
    f.close();
    return out;
  }
  out.resize(n);
  size_t got = 0;
  while (got < n) {
    const int r = f.read(out.data() + got, n - got);
    if (r <= 0) break;
    got += static_cast<size_t>(r);
  }
  f.close();
  out.resize(got);
  return out;
}

bool writeBinary(const std::string& path, const std::vector<uint8_t>& data) {
  HalFile f;
  if (!Storage.openFileForWrite("VAULT", path, f)) return false;
  const size_t w = f.write(data.data(), data.size());
  f.close();
  return w == data.size();
}

// Derive a filesystem-safe base name from the note's first line.
std::string sanitizeTitle(const std::string& text) {
  std::string firstLine = text.substr(0, text.find('\n'));
  std::string out;
  for (char c : firstLine) {
    if (std::isalnum(static_cast<unsigned char>(c)) || c == ' ' || c == '-' || c == '_') {
      out.push_back(c);
    }
    if (out.size() >= 24) break;
  }
  // Trim surrounding spaces.
  while (!out.empty() && out.front() == ' ') out.erase(out.begin());
  while (!out.empty() && out.back() == ' ') out.pop_back();
  if (out.empty()) out = "note";
  return out;
}
}  // namespace

void VaultActivity::onEnter() {
  Activity::onEnter();
  state = State::Locked;
  pendingPrompt = true;
  retryAfterWrong = false;
  selectedIndex = 0;
  requestUpdate();
}

void VaultActivity::onExit() {
  wipeKey();
  Activity::onExit();
}

void VaultActivity::wipeKey() {
  if (keyValid) {
    VaultCrypto::wipe(key, VaultCrypto::KEY_LEN);
    keyValid = false;
  }
}

void VaultActivity::launchPinPrompt() {
  const bool firstRun = !VAULT_CONFIG.isConfigured();
  std::string title;
  if (retryAfterWrong) {
    title = "Wrong PIN - try again";
  } else {
    title = firstRun ? "Set a vault PIN" : "Enter vault PIN";
  }

  startActivityForResult(
      std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, title, "", 32, InputType::Password),
      [this](const ActivityResult& result) { handlePinResult(result); });
}

void VaultActivity::handlePinResult(const ActivityResult& result) {
  if (result.isCancelled) {
    finish();  // cancelling the PIN prompt leaves the vault
    return;
  }

  const std::string pin = std::get<KeyboardResult>(result.data).text;
  const bool firstRun = !VAULT_CONFIG.isConfigured();

  wipeKey();
  const bool ok = firstRun ? VAULT_CONFIG.createVault(pin, key) : VAULT_CONFIG.verifyPin(pin, key);

  if (ok) {
    keyValid = true;
    state = State::Unlocked;
    retryAfterWrong = false;
    selectedIndex = 0;
    loadEntries();
    LOG_INF("VAULT", firstRun ? "Vault created + unlocked" : "Vault unlocked");
    requestUpdate();
  } else {
    retryAfterWrong = !firstRun || !pin.empty();  // empty PIN on first run = re-prompt quietly
    pendingPrompt = true;
    requestUpdate();
  }
}

void VaultActivity::loadEntries() {
  fileNames.clear();
  displayNames.clear();
  Storage.mkdir(VAULT_DIR);
  for (const String& name : Storage.listFiles(VAULT_DIR)) {
    std::string n(name.c_str());
    if (!hasPbvExtension(n)) continue;
    displayNames.push_back(n.substr(0, n.size() - 4));
    fileNames.push_back(std::move(n));
  }
}

std::string VaultActivity::makeUniqueFileName(const std::string& title) const {
  const std::string base = sanitizeTitle(title);
  std::string candidate = base + PBV_EXT;
  int n = 2;
  while (Storage.exists((std::string(VAULT_DIR) + "/" + candidate).c_str())) {
    candidate = base + " " + std::to_string(n++) + PBV_EXT;
  }
  return candidate;
}

bool VaultActivity::saveEncrypted(const std::string& title, const std::string& text) {
  if (text.empty() || !keyValid) return false;

  std::vector<uint8_t> blob;
  if (!VaultCrypto::encrypt(key, reinterpret_cast<const uint8_t*>(text.data()), text.size(), blob)) {
    LOG_ERR("VAULT", "Encrypt failed");
    return false;
  }

  const std::string fileName = makeUniqueFileName(title);
  const std::string path = std::string(VAULT_DIR) + "/" + fileName;
  if (!writeBinary(path, blob)) {
    LOG_ERR("VAULT", "Write failed: %s", path.c_str());
    return false;
  }
  LOG_INF("VAULT", "Saved: %s (%u B)", fileName.c_str(), (unsigned)blob.size());

  loadEntries();
  // Select the newly created entry (offset by the action rows).
  for (size_t i = 0; i < fileNames.size(); ++i) {
    if (fileNames[i] == fileName) {
      selectedIndex = static_cast<int>(i) + ACTION_ROWS;
      break;
    }
  }
  return true;
}

void VaultActivity::createNoteFromText(const std::string& text) {
  // Title from the note's first line.
  saveEncrypted(text, text);
}

void VaultActivity::importMarkdownFile(const std::string& path) {
  if (!keyValid) return;

  std::vector<uint8_t> raw = readBinary(path);
  if (raw.empty()) {
    LOG_ERR("VAULT", "Import read failed/empty: %s", path.c_str());
    return;
  }
  std::string text(reinterpret_cast<const char*>(raw.data()), raw.size());

  // Title from the source file's base name (strip path and .md extension).
  std::string base = path.substr(path.find_last_of('/') + 1);
  const size_t dot = base.rfind('.');
  if (dot != std::string::npos) base = base.substr(0, dot);

  if (saveEncrypted(base, text)) {
    // Move semantics: delete the plaintext original from the SD card so no
    // unencrypted copy is left behind.
    if (Storage.remove(path.c_str())) {
      LOG_INF("VAULT", "Moved into vault (deleted source): %s", path.c_str());
    } else {
      LOG_ERR("VAULT", "Encrypted OK but failed to delete source: %s", path.c_str());
    }
  }
}

void VaultActivity::openEntry(int entryIdx) {
  if (!keyValid || entryIdx < 0 || entryIdx >= static_cast<int>(fileNames.size())) return;

  const std::string path = std::string(VAULT_DIR) + "/" + fileNames[entryIdx];
  std::vector<uint8_t> blob = readBinary(path);
  if (blob.empty()) {
    LOG_ERR("VAULT", "Read failed: %s", path.c_str());
    return;
  }

  std::vector<uint8_t> plain;
  if (!VaultCrypto::decrypt(key, blob.data(), blob.size(), plain)) {
    LOG_ERR("VAULT", "Decrypt failed: %s", path.c_str());
    return;
  }

  std::string text(reinterpret_cast<const char*>(plain.data()), plain.size());
  if (!plain.empty()) VaultCrypto::wipe(plain.data(), plain.size());

  startActivityForResult(
      std::make_unique<VaultReaderActivity>(renderer, mappedInput, std::move(text), displayNames[entryIdx]),
      [this](const ActivityResult&) { requestUpdate(); });
}

void VaultActivity::onConfirm() {
  if (selectedIndex == 0) {
    // "+ New note" — type the note on-screen, then encrypt it on return.
    startActivityForResult(
        std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, "New note", "", MAX_NOTE_CHARS, InputType::Text),
        [this](const ActivityResult& result) {
          if (!result.isCancelled) {
            createNoteFromText(std::get<KeyboardResult>(result.data).text);
          }
          requestUpdate();
        });
  } else if (selectedIndex == 1) {
    // "+ Import .md from SD" — recursively list every .md on the card; selecting one
    // encrypts it into the vault and deletes the plaintext original (a move).
    startActivityForResult(std::make_unique<VaultImportActivity>(renderer, mappedInput),
                           [this](const ActivityResult& result) {
                             if (!result.isCancelled) {
                               importMarkdownFile(std::get<FilePathResult>(result.data).path);
                             }
                             requestUpdate();
                           });
  } else {
    openEntry(selectedIndex - ACTION_ROWS);
  }
}

void VaultActivity::loop() {
  if (pendingPrompt) {
    pendingPrompt = false;
    launchPinPrompt();
    return;
  }

  if (state != State::Unlocked) return;

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    onConfirm();
    return;
  }

  const int count = getItemCount();
  buttonNavigator.onNext([this, count] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, count);
    requestUpdate();
  });
  buttonNavigator.onPrevious([this, count] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, count);
    requestUpdate();
  });
}

void VaultActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "My Vault");

  if (state != State::Unlocked) {
    const int lineH = renderer.getLineHeight(UI_10_FONT_ID);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - lineH, "Unlocking vault...", true,
                              EpdFontFamily::REGULAR);
    renderer.displayBuffer();
    return;
  }

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;

  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, getItemCount(), selectedIndex,
      [this](int index) -> std::string {
        if (index == 0) return "+ New note";
        if (index == 1) return "+ Import .md from SD";
        return displayNames[index - ACTION_ROWS];
      });

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
