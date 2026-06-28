#include "VaultConfigStore.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>
#include <mbedtls/base64.h>

#include <cstring>

VaultConfigStore VaultConfigStore::instance;

namespace {
constexpr char VAULT_FILE_JSON[] = "/.crosspoint/vault.json";

// base64-encode raw bytes into a std::string (no NUL issues; not secret data).
std::string b64Encode(const uint8_t* data, size_t len) {
  size_t outLen = 0;
  // First call sizes the output buffer.
  mbedtls_base64_encode(nullptr, 0, &outLen, data, len);
  std::string out(outLen, '\0');
  if (mbedtls_base64_encode(reinterpret_cast<uint8_t*>(out.data()), outLen, &outLen, data, len) != 0) {
    return {};
  }
  out.resize(outLen);  // drop trailing NUL counted by mbedtls
  return out;
}

// base64-decode into a fixed-size buffer; returns true only if the decoded
// length matches `expectLen` exactly.
bool b64DecodeExact(const char* b64, uint8_t* out, size_t expectLen) {
  if (!b64) return false;
  const size_t inLen = strlen(b64);
  size_t outLen = 0;
  if (mbedtls_base64_decode(out, expectLen, &outLen, reinterpret_cast<const uint8_t*>(b64), inLen) != 0) {
    return false;
  }
  return outLen == expectLen;
}
}  // namespace

bool VaultConfigStore::loadFromFile() {
  if (loaded) return configured;
  loaded = true;
  configured = false;

  if (!Storage.exists(VAULT_FILE_JSON)) {
    return false;
  }

  String json = Storage.readFile(VAULT_FILE_JSON);
  if (json.isEmpty()) {
    return false;
  }

  JsonDocument doc;
  if (deserializeJson(doc, json.c_str()) != DeserializationError::Ok) {
    LOG_ERR("VAULT", "vault.json parse failed");
    return false;
  }

  const uint32_t fileIters = doc["iters"] | 0u;
  const char* saltB64 = doc["salt"] | static_cast<const char*>(nullptr);
  const char* verifierB64 = doc["verifier"] | static_cast<const char*>(nullptr);

  if (fileIters == 0 || !saltB64 || !verifierB64) {
    LOG_ERR("VAULT", "vault.json missing fields");
    return false;
  }
  if (!b64DecodeExact(saltB64, salt, VaultCrypto::SALT_LEN) ||
      !b64DecodeExact(verifierB64, verifier, VaultCrypto::VERIFIER_LEN)) {
    LOG_ERR("VAULT", "vault.json bad salt/verifier encoding");
    return false;
  }

  iters = fileIters;
  configured = true;
  LOG_INF("VAULT", "Loaded vault config (iters=%u)", iters);
  return true;
}

bool VaultConfigStore::isConfigured() {
  loadFromFile();
  return configured;
}

bool VaultConfigStore::saveToFile() const {
  Storage.mkdir("/.crosspoint");

  JsonDocument doc;
  doc["version"] = 1;
  doc["iters"] = iters;
  doc["salt"] = b64Encode(salt, VaultCrypto::SALT_LEN);
  doc["verifier"] = b64Encode(verifier, VaultCrypto::VERIFIER_LEN);

  String out;
  serializeJson(doc, out);
  if (!Storage.writeFile(VAULT_FILE_JSON, out)) {
    LOG_ERR("VAULT", "Failed to write vault.json");
    return false;
  }
  return true;
}

bool VaultConfigStore::createVault(const std::string& pin, uint8_t outKey[VaultCrypto::KEY_LEN]) {
  if (pin.empty()) return false;

  VaultCrypto::randomBytes(salt, VaultCrypto::SALT_LEN);
  iters = DEFAULT_ITERS;

  if (!VaultCrypto::deriveKey(pin, salt, VaultCrypto::SALT_LEN, iters, outKey)) {
    LOG_ERR("VAULT", "createVault: deriveKey failed");
    return false;
  }
  if (!VaultCrypto::makeVerifier(outKey, verifier)) {
    VaultCrypto::wipe(outKey, VaultCrypto::KEY_LEN);
    LOG_ERR("VAULT", "createVault: verifier failed");
    return false;
  }

  if (!saveToFile()) {
    VaultCrypto::wipe(outKey, VaultCrypto::KEY_LEN);
    return false;
  }

  loaded = true;
  configured = true;
  LOG_INF("VAULT", "Vault created (iters=%u)", iters);
  return true;
}

bool VaultConfigStore::verifyPin(const std::string& pin, uint8_t outKey[VaultCrypto::KEY_LEN]) {
  if (!isConfigured()) return false;

  uint8_t candidate[VaultCrypto::VERIFIER_LEN];
  if (!VaultCrypto::deriveKey(pin, salt, VaultCrypto::SALT_LEN, iters, outKey)) {
    return false;
  }
  if (!VaultCrypto::makeVerifier(outKey, candidate)) {
    VaultCrypto::wipe(outKey, VaultCrypto::KEY_LEN);
    return false;
  }

  const bool ok = VaultCrypto::ctEquals(candidate, verifier, VaultCrypto::VERIFIER_LEN);
  VaultCrypto::wipe(candidate, sizeof(candidate));
  if (!ok) {
    VaultCrypto::wipe(outKey, VaultCrypto::KEY_LEN);
  }
  return ok;
}
