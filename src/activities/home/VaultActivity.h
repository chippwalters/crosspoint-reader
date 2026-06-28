#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "crypto/VaultCrypto.h"
#include "util/ButtonNavigator.h"

/**
 * "My Vault" — PAPERBIT's password-protected section.
 *
 * Locked: prompts for a PIN via the on-screen keyboard (Password mode). First
 * run sets the PIN ("Set a vault PIN"); thereafter it verifies ("Enter vault
 * PIN") against the on-disk verifier — the PIN itself is never stored.
 *
 * Unlocked: shows the vault note list. The derived AES-256 key lives only in
 * RAM for the lifetime of this activity and is wiped on exit. Notes are created
 * on-device via the keyboard, encrypted (AES-256-GCM) into /Vault/<name>.pbv,
 * and decrypted in-memory only for viewing (see VaultReaderActivity). No
 * plaintext ever touches the SD card and vault entries never appear in Recents.
 */
class VaultActivity final : public Activity {
  enum class State { Locked, Unlocked };
  State state = State::Locked;

  bool pendingPrompt = true;     // launch (or relaunch) the PIN keyboard on next loop
  bool retryAfterWrong = false;  // last attempt was a wrong PIN
  bool keyValid = false;
  uint8_t key[VaultCrypto::KEY_LEN] = {0};

  ButtonNavigator buttonNavigator;
  int selectedIndex = 0;
  std::vector<std::string> fileNames;  // on-disk names, e.g. "Groceries.pbv"
  std::vector<std::string> displayNames;  // shown names, e.g. "Groceries"

  // Rows 0 = "+ New note", 1 = "+ Import .md from SD"; entries follow.
  static constexpr int ACTION_ROWS = 2;
  int getItemCount() const { return ACTION_ROWS + static_cast<int>(fileNames.size()); }

  void launchPinPrompt();
  void handlePinResult(const ActivityResult& result);
  void wipeKey();

  void loadEntries();
  void onConfirm();
  void createNoteFromText(const std::string& text);
  void importMarkdownFile(const std::string& path);
  bool saveEncrypted(const std::string& title, const std::string& text);
  void openEntry(int entryIdx);
  std::string makeUniqueFileName(const std::string& title) const;

 public:
  explicit VaultActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Vault", renderer, mappedInput) {}
  ~VaultActivity() override { wipeKey(); }

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
