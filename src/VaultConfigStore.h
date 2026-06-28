#pragma once
#include <cstdint>
#include <string>

#include "crypto/VaultCrypto.h"

/**
 * Singleton storing the PAPERBIT "My Vault" configuration on the SD card
 * (/.crosspoint/vault.json).
 *
 * The vault PIN is never stored. We persist only:
 *   - salt      (random, per-device; not secret)
 *   - iters     (PBKDF2 iteration count)
 *   - verifier  (HMAC-SHA256(key, "PAPERBIT-VAULT-VERIFY"); lets us check a PIN
 *                without storing it, and reveals nothing about the key)
 *
 * The derived AES key lives only in RAM while the vault is unlocked and is
 * wiped on exit. Vault files (.pbv) are decrypted in-memory only.
 */
class VaultConfigStore {
 private:
  static VaultConfigStore instance;

  bool loaded = false;
  bool configured = false;
  uint8_t salt[VaultCrypto::SALT_LEN] = {0};
  uint32_t iters = 0;
  uint8_t verifier[VaultCrypto::VERIFIER_LEN] = {0};

  // ~250 ms on the ESP32-C3 with HW SHA acceleration; good enough for casual
  // privacy while staying responsive on a 6-button device.
  static constexpr uint32_t DEFAULT_ITERS = 30000;

  VaultConfigStore() = default;

  bool saveToFile() const;

 public:
  VaultConfigStore(const VaultConfigStore&) = delete;
  VaultConfigStore& operator=(const VaultConfigStore&) = delete;

  static VaultConfigStore& getInstance() { return instance; }

  // Loads vault.json if present (lazy; safe to call repeatedly).
  bool loadFromFile();

  // True once a vault has been created (PIN set).
  bool isConfigured();

  // First-run: generate salt, derive the key from `pin`, store the verifier,
  // and persist vault.json. On success fills outKey (caller must wipe it).
  bool createVault(const std::string& pin, uint8_t outKey[VaultCrypto::KEY_LEN]);

  // Verify `pin` against the stored verifier. On success fills outKey (caller
  // must wipe it) and returns true; returns false on wrong PIN or no vault.
  bool verifyPin(const std::string& pin, uint8_t outKey[VaultCrypto::KEY_LEN]);
};

#define VAULT_CONFIG VaultConfigStore::getInstance()
