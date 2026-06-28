#include "VaultCrypto.h"

#include <cstring>

#include <esp_random.h>
#include <mbedtls/gcm.h>
#include <mbedtls/md.h>
#include <mbedtls/pkcs5.h>
#include <mbedtls/platform_util.h>

namespace VaultCrypto {

static const char MAGIC[4] = {'P', 'B', 'V', '1'};
static const char VERIFY_LABEL[] = "PAPERBIT-VAULT-VERIFY";

void randomBytes(uint8_t* buf, size_t len) { esp_fill_random(buf, len); }

void wipe(void* buf, size_t len) { mbedtls_platform_zeroize(buf, len); }

bool deriveKey(const std::string& pin, const uint8_t* salt, size_t saltLen, uint32_t iters,
               uint8_t outKey[KEY_LEN]) {
  return mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA256, reinterpret_cast<const uint8_t*>(pin.data()),
                                       pin.size(), salt, saltLen, iters, KEY_LEN, outKey) == 0;
}

bool makeVerifier(const uint8_t key[KEY_LEN], uint8_t outVerifier[VERIFIER_LEN]) {
  const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (!info) return false;
  return mbedtls_md_hmac(info, key, KEY_LEN, reinterpret_cast<const uint8_t*>(VERIFY_LABEL),
                         sizeof(VERIFY_LABEL) - 1, outVerifier) == 0;
}

bool ctEquals(const uint8_t* a, const uint8_t* b, size_t len) {
  // constant-time: accumulate all byte differences, no early exit (mbedtls_ct_memcmp
  // isn't exported in ESP-IDF's mbedtls build).
  uint8_t diff = 0;
  for (size_t i = 0; i < len; i++) diff |= static_cast<uint8_t>(a[i] ^ b[i]);
  return diff == 0;
}

bool encrypt(const uint8_t key[KEY_LEN], const uint8_t* plain, size_t plainLen,
             std::vector<uint8_t>& out) {
  uint8_t nonce[NONCE_LEN];
  uint8_t tag[TAG_LEN];
  randomBytes(nonce, NONCE_LEN);
  std::vector<uint8_t> cipher(plainLen);

  mbedtls_gcm_context ctx;
  mbedtls_gcm_init(&ctx);
  bool ok = false;
  if (mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key, KEY_LEN * 8) == 0) {
    ok = mbedtls_gcm_crypt_and_tag(&ctx, MBEDTLS_GCM_ENCRYPT, plainLen, nonce, NONCE_LEN, nullptr, 0,
                                   plain, cipher.data(), TAG_LEN, tag) == 0;
  }
  mbedtls_gcm_free(&ctx);

  if (ok) {
    out.clear();
    out.reserve(HEADER_LEN + plainLen);
    out.insert(out.end(), MAGIC, MAGIC + 4);
    out.push_back(FORMAT_VERSION);
    out.insert(out.end(), nonce, nonce + NONCE_LEN);
    out.insert(out.end(), tag, tag + TAG_LEN);
    out.insert(out.end(), cipher.begin(), cipher.end());
  }
  if (!cipher.empty()) wipe(cipher.data(), cipher.size());
  return ok;
}

bool decrypt(const uint8_t key[KEY_LEN], const uint8_t* c, size_t clen, std::vector<uint8_t>& out) {
  out.clear();
  if (clen < HEADER_LEN) return false;
  if (memcmp(c, MAGIC, 4) != 0) return false;
  // c[4] = version (only 1 today)
  const uint8_t* nonce = c + 5;
  const uint8_t* tag = c + 5 + NONCE_LEN;
  const uint8_t* cipher = c + HEADER_LEN;
  const size_t cipherLen = clen - HEADER_LEN;

  out.assign(cipherLen, 0);
  mbedtls_gcm_context ctx;
  mbedtls_gcm_init(&ctx);
  bool ok = false;
  if (mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key, KEY_LEN * 8) == 0) {
    // auth_decrypt verifies the tag internally; non-zero return = wrong key / tampered.
    ok = mbedtls_gcm_auth_decrypt(&ctx, cipherLen, nonce, NONCE_LEN, nullptr, 0, tag, TAG_LEN,
                                  cipher, out.data()) == 0;
  }
  mbedtls_gcm_free(&ctx);
  if (!ok && !out.empty()) {
    wipe(out.data(), out.size());
    out.clear();
  }
  return ok;
}

std::string selfTest() {
  const std::string pin = "1234";
  const std::string badPin = "9999";
  const char* msg = "# Secret\nThis is a **vault** markdown test.\n";
  const size_t msgLen = strlen(msg);

  uint8_t salt[SALT_LEN];
  randomBytes(salt, SALT_LEN);
  uint8_t key[KEY_LEN], badKey[KEY_LEN], verifier[VERIFIER_LEN], verifier2[VERIFIER_LEN];

  if (!deriveKey(pin, salt, SALT_LEN, 20000, key)) return "VAULT selftest: deriveKey FAIL";
  if (!makeVerifier(key, verifier)) return "VAULT selftest: verifier FAIL";
  // verifier reproducible
  deriveKey(pin, salt, SALT_LEN, 20000, badKey);  // same pin -> same key
  makeVerifier(badKey, verifier2);
  if (!ctEquals(verifier, verifier2, VERIFIER_LEN)) return "VAULT selftest: verifier not stable";

  std::vector<uint8_t> blob, plain, plainBad;
  if (!encrypt(key, reinterpret_cast<const uint8_t*>(msg), msgLen, blob))
    return "VAULT selftest: encrypt FAIL";
  if (!decrypt(key, blob.data(), blob.size(), plain)) return "VAULT selftest: decrypt FAIL";
  if (plain.size() != msgLen || memcmp(plain.data(), msg, msgLen) != 0)
    return "VAULT selftest: roundtrip MISMATCH";

  // wrong PIN must fail (tag mismatch), never return plaintext
  deriveKey(badPin, salt, SALT_LEN, 20000, badKey);
  bool wrongAccepted = decrypt(badKey, blob.data(), blob.size(), plainBad);

  wipe(key, KEY_LEN);
  wipe(badKey, KEY_LEN);
  if (wrongAccepted) return "VAULT selftest: WRONG PIN ACCEPTED (FAIL)";
  return "VAULT selftest: OK (roundtrip + wrong-pin rejected, blob=" + std::to_string(blob.size()) + "B)";
}

}  // namespace VaultCrypto
