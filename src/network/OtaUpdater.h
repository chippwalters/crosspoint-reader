#pragma once

#include <string>

class OtaUpdater {
  bool updateAvailable = false;
  std::string latestVersion;
  std::string otaUrl;
  size_t otaSize = 0;
  size_t processedSize = 0;
  size_t totalSize = 0;

 public:
  using ProgressCallback = void (*)(void* ctx);

  enum OtaUpdaterError {
    OK = 0,
    NO_UPDATE,
    HTTP_ERROR,
    JSON_PARSE_ERROR,
    UPDATE_OLDER_ERROR,
    INTERNAL_UPDATE_ERROR,
    OOM_ERROR,
  };

  size_t getOtaSize() const { return otaSize; }

  size_t getProcessedSize() const { return processedSize; }

  size_t getTotalSize() const { return totalSize; }

  OtaUpdater() = default;
  bool isUpdateNewer() const;
  const std::string& getLatestVersion() const;
  const std::string& getOtaUrl() const { return otaUrl; }
  OtaUpdaterError checkForUpdate();
  OtaUpdaterError installUpdate(ProgressCallback onProgress = nullptr, void* ctx = nullptr);

  // --- Reboot-to-install flow -------------------------------------------------------------
  // In-activity installs fail on the C3: after any earlier TLS session in the boot, the heap's
  // largest free block collapses below what a second mbedtls handshake needs, and cert
  // verification fails with a bogus -0x3000 (observed 2026-07-07: first TLS OK at ~65 KB
  // largest; second fails at ~43 KB). So the confirmed update is persisted to SD and installed
  // at EARLY BOOT, where the handshake is the boot's first (and only) TLS connection.
  // Seed this updater from a persisted pending update (skips checkForUpdate()).
  void seedUpdate(const std::string& url, const std::string& version, size_t size);
  static bool savePending(const std::string& url, const std::string& version, size_t size);
  static bool loadPending(std::string& url, std::string& version, size_t& size);
  static void clearPending();
};
