#pragma once
// VaultCrypto — PAPERBIT "My Vault" file encryption for CrossPoint (ESP32-C3).
// PBKDF2-HMAC-SHA256 (PIN + salt) -> 32-byte master key; per-file AES-256-GCM (HW-accelerated)
// with a fresh random 96-bit nonce and a 128-bit auth tag. Whole-buffer (in-memory) for the
// Markdown MVP; the container is versioned so chunked AEAD can be added for EPUB later.
//
// File container layout (/Vault/<name>.pbv):
//   "PBV1"(4) | version(1) | nonce(12) | tag(16) | ciphertext(...)
// Vault metadata (/.crosspoint/vault.json) holds salt + iterations + a verifier (NOT the PIN).
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace VaultCrypto {

constexpr size_t KEY_LEN = 32;       // AES-256
constexpr size_t SALT_LEN = 16;
constexpr size_t NONCE_LEN = 12;     // GCM standard nonce
constexpr size_t TAG_LEN = 16;
constexpr size_t VERIFIER_LEN = 32;  // HMAC-SHA256
constexpr uint8_t FORMAT_VERSION = 1;
constexpr size_t HEADER_LEN = 4 + 1 + NONCE_LEN + TAG_LEN;  // magic+ver+nonce+tag

// CSPRNG (hardware RNG).
void randomBytes(uint8_t* buf, size_t len);
// Zeroize sensitive buffers (won't be optimized away).
void wipe(void* buf, size_t len);

// PBKDF2-HMAC-SHA256(pin, salt, iters) -> outKey[KEY_LEN]. Returns true on success.
bool deriveKey(const std::string& pin, const uint8_t* salt, size_t saltLen, uint32_t iters,
               uint8_t outKey[KEY_LEN]);
// verifier = HMAC-SHA256(key, "PAPERBIT-VAULT-VERIFY"). Lets us check the PIN without storing it.
bool makeVerifier(const uint8_t key[KEY_LEN], uint8_t outVerifier[VERIFIER_LEN]);
// Constant-time equality.
bool ctEquals(const uint8_t* a, const uint8_t* b, size_t len);

// Encrypt plaintext into a .pbv container (out). Returns true on success.
bool encrypt(const uint8_t key[KEY_LEN], const uint8_t* plain, size_t plainLen,
             std::vector<uint8_t>& out);
// Decrypt a .pbv container into plaintext (out). Returns true ONLY if the tag verifies
// (wrong PIN or tampering -> false, out cleared). Plaintext is never exposed on failure.
bool decrypt(const uint8_t key[KEY_LEN], const uint8_t* container, size_t containerLen,
             std::vector<uint8_t>& out);

// On-device smoke test: round-trip encrypt/decrypt + wrong-key rejection + verifier.
// Returns a one-line result string for serial logging. No secrets in the output.
std::string selfTest();

}  // namespace VaultCrypto
