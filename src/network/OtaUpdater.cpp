#include "OtaUpdater.h"

// clang-format off
// HttpDownloader.h pulls Arduino/SdFat, whose macros collide with lwip's
// ip4_addr.h unless seen before esp_http_client (which includes lwip). Pin this
// order; clang-format would otherwise sort the local header last and break the
// build.
#include "HttpDownloader.h"
#include <Logging.h>
#include <ReleaseJsonParser.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_https_ota.h>
#include <esp_wifi.h>
// clang-format on

#include <ArduinoJson.h>

#include <string>

namespace {
// Paperbit fork: the device's built-in Wi-Fi OTA checks OUR published release feed instead of
// upstream CrossPoint's GitHub releases. release.json is a GitHub-releases-shaped feed emitted by
// paperbit/webtool/publish-firmware.ps1 (tag_name + an asset named "firmware.bin" whose
// browser_download_url points at the versioned binary). ReleaseJsonParser reads it unchanged.
//
// Plain HTTP by design (server serves it without a redirect): the in-session TLS handshake is
// heap-marginal on the C3 — especially on the ble build (~30 KB less total heap) — and fails
// with PK-verify/MPI-alloc errors when the largest free block is squeezed (field-measured
// 2026-07-07). The 302-byte check needs no confidentiality; the firmware IMAGE download (early
// boot) is ALSO plain http — publish-firmware.ps1 emits http:// browser_download_urls. The
// device has no firmware signature enforcement anyway (see gen_manifest.py) — image validation
// + the A/B slots are the rollback safety.
constexpr char latestReleaseUrl[] = "http://www.widgetgadget.com/cw1/Paperbit/release.json";

// Persisted confirmed-update for the reboot-to-install flow (see OtaUpdater.h).
constexpr char OTA_PENDING_FILE[] = "/.crosspoint/ota_pending.json";

esp_err_t http_client_set_header_cb(esp_http_client_handle_t http_client) {
  return esp_http_client_set_header(http_client, "User-Agent", "CrossPoint-ESP32-" CROSSPOINT_VERSION);
}
}  // namespace

OtaUpdater::OtaUpdaterError OtaUpdater::checkForUpdate() {
  LOG_DBG("OTA", "Checking for update (current: %s)", CROSSPOINT_VERSION);

  // Stream the ~32KB release JSON straight into the parser as it arrives.
  // Buffering the whole body in a std::string would add a growing allocation
  // on top of the TLS session's heap during the fetch; with -fno-exceptions an
  // OOM there aborts. fetchUrl handles the verified-https GET, redirects, and
  // User-Agent (see HttpDownloader).
  ReleaseJsonParser releaseParser;
  const bool ok = HttpDownloader::fetchUrl(latestReleaseUrl, [&releaseParser](const uint8_t* data, size_t len) {
    releaseParser.feed(reinterpret_cast<const char*>(data), len);
    return true;
  });
  if (!ok) {
    LOG_ERR("OTA", "Release check fetch failed");
    return HTTP_ERROR;
  }

  LOG_DBG("OTA", "Parser results: tag=%s firmware=%s", releaseParser.foundTag() ? "yes" : "no",
          releaseParser.foundFirmware() ? "yes" : "no");

  if (!releaseParser.foundTag()) {
    LOG_ERR("OTA", "No tag_name in release JSON");
    return JSON_PARSE_ERROR;
  }

  if (!releaseParser.foundFirmware()) {
    LOG_ERR("OTA", "No firmware.bin asset found");
    return NO_UPDATE;
  }

  latestVersion = releaseParser.getTagName();
  otaUrl = releaseParser.getFirmwareUrl();
  otaSize = releaseParser.getFirmwareSize();
  totalSize = otaSize;
  updateAvailable = true;

  LOG_DBG("OTA", "Found update: tag=%s size=%zu", latestVersion.c_str(), otaSize);
  LOG_DBG("OTA", "Firmware URL: %s", otaUrl.c_str());
  return OK;
}

bool OtaUpdater::isUpdateNewer() const {
  if (!updateAvailable || latestVersion.empty() || latestVersion == CROSSPOINT_VERSION) {
    return false;
  }

  int currentMajor, currentMinor, currentPatch;
  int latestMajor, latestMinor, latestPatch;

  const auto currentVersion = CROSSPOINT_VERSION;

  // semantic version check (only match on 3 segments)
  sscanf(latestVersion.c_str(), "%d.%d.%d", &latestMajor, &latestMinor, &latestPatch);
  sscanf(currentVersion, "%d.%d.%d", &currentMajor, &currentMinor, &currentPatch);

  /*
   * Compare major versions.
   * If they differ, return true if latest major version greater than current major version
   * otherwise return false.
   */
  if (latestMajor != currentMajor) return latestMajor > currentMajor;

  /*
   * Compare minor versions.
   * If they differ, return true if latest minor version greater than current minor version
   * otherwise return false.
   */
  if (latestMinor != currentMinor) return latestMinor > currentMinor;

  /*
   * Check patch versions.
   */
  if (latestPatch != currentPatch) return latestPatch > currentPatch;

  // If we reach here, it means all segments are equal.
  // One final check, if we're on an RC build (contains "-rc"), we should consider the latest version as newer even if
  // the segments are equal, since RC builds are pre-release versions.
  if (strstr(currentVersion, "-rc") != nullptr) {
    return true;
  }

  return false;
}

const std::string& OtaUpdater::getLatestVersion() const { return latestVersion; }

OtaUpdater::OtaUpdaterError OtaUpdater::installUpdate(ProgressCallback onProgress, void* ctx) {
  if (!isUpdateNewer()) {
    return UPDATE_OLDER_ERROR;
  }

  esp_https_ota_handle_t ota_handle = NULL;
  esp_err_t esp_err;

  esp_http_client_config_t client_config = {
      .url = otaUrl.c_str(),
      .timeout_ms = 15000,
      // 4096 holds the github->CDN redirect headers (the 512 default truncates
      // them); TX only carries our GET. Both are contiguous blocks contending
      // with the TLS handshake on a tight internal arena, so keep them minimal.
      .buffer_size = 4096,
      .buffer_size_tx = 1024,
      // NEVER set skip_cert_common_name_check here: esp-tls implements it as
      // mbedtls_ssl_set_hostname(ssl, NULL), which also disables SNI — a name-based
      // vhost then serves its DEFAULT certificate and the CA bundle correctly rejects
      // it ("Failed to verify certificate", -0x3000). Field-diagnosed 2026-07-07.
      .crt_bundle_attach = esp_crt_bundle_attach,
      .keep_alive_enable = true,
  };

  esp_https_ota_config_t ota_config = {
      .http_config = &client_config,
      .http_client_init_cb = http_client_set_header_cb,
  };

  /* For better timing and connectivity, we disable power saving for WiFi */
  esp_wifi_set_ps(WIFI_PS_NONE);

  // Heap diagnostics: the install's TLS handshake needs contiguous internal RAM on top of
  // whatever the caller left resident. mbedtls allocation failures during the handshake
  // surface as bogus "certificate verification failed" (-0x3000) errors, so log the real
  // conditions up front (2026-07-07 field failure: check OK, install handshake failed).
  LOG_INF("OTA", "install pre-begin heap: free=%u largest=%u", (unsigned)ESP.getFreeHeap(),
          (unsigned)ESP.getMaxAllocHeap());

  esp_err = esp_https_ota_begin(&ota_config, &ota_handle);
  if (esp_err != ESP_OK) {
    LOG_ERR("OTA", "HTTP OTA Begin Failed: %s (heap free=%u largest=%u)", esp_err_to_name(esp_err),
            (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
    return INTERNAL_UPDATE_ERROR;
  }
  LOG_INF("OTA", "install begin OK: heap free=%u largest=%u", (unsigned)ESP.getFreeHeap(),
          (unsigned)ESP.getMaxAllocHeap());

  int lastReportedPct = -1;
  do {
    esp_err = esp_https_ota_perform(ota_handle);
    processedSize = esp_https_ota_get_image_len_read(ota_handle);
    // Fire the callback only on whole-percent change. Without this it fired
    // every ~100ms perform iteration, waking the render task whose framebuffer
    // work contends with TLS on the same internal arena. E-ink can't repaint
    // faster than a percent tick anyway.
    if (onProgress && totalSize > 0) {
      const int pct = static_cast<int>(static_cast<uint64_t>(processedSize) * 100 / totalSize);
      if (pct != lastReportedPct) {
        lastReportedPct = pct;
        onProgress(ctx);
      }
    }
    delay(100);  // TODO: should we replace this with something better?
  } while (esp_err == ESP_ERR_HTTPS_OTA_IN_PROGRESS);

  /* Return back to default power saving for WiFi in case of failing */
  esp_wifi_set_ps(WIFI_PS_MIN_MODEM);

  if (esp_err != ESP_OK) {
    LOG_ERR("OTA", "esp_https_ota_perform Failed: %s", esp_err_to_name(esp_err));
    esp_https_ota_finish(ota_handle);
    return HTTP_ERROR;
  }

  if (!esp_https_ota_is_complete_data_received(ota_handle)) {
    LOG_ERR("OTA", "esp_https_ota_is_complete_data_received Failed: %s", esp_err_to_name(esp_err));
    esp_https_ota_finish(ota_handle);
    return INTERNAL_UPDATE_ERROR;
  }

  esp_err = esp_https_ota_finish(ota_handle);
  if (esp_err != ESP_OK) {
    LOG_ERR("OTA", "esp_https_ota_finish Failed: %s", esp_err_to_name(esp_err));
    return INTERNAL_UPDATE_ERROR;
  }

  LOG_INF("OTA", "Update completed");
  return OK;
}

// --- Reboot-to-install persistence (see header) ------------------------------------------------

void OtaUpdater::seedUpdate(const std::string& url, const std::string& version, size_t size) {
  otaUrl = url;
  latestVersion = version;
  otaSize = size;
  totalSize = size;
  processedSize = 0;
  updateAvailable = true;
}

bool OtaUpdater::savePending(const std::string& url, const std::string& version, size_t size) {
  JsonDocument doc;
  doc["url"] = url;
  doc["version"] = version;
  doc["size"] = static_cast<uint32_t>(size);
  String json;
  serializeJson(doc, json);
  Storage.mkdir("/.crosspoint");
  const bool ok = Storage.writeFile(OTA_PENDING_FILE, json);
  if (!ok) LOG_ERR("OTA", "savePending: write failed for %s", OTA_PENDING_FILE);
  return ok;
}

bool OtaUpdater::loadPending(std::string& url, std::string& version, size_t& size) {
  if (!Storage.exists(OTA_PENDING_FILE)) return false;
  const String json = Storage.readFile(OTA_PENDING_FILE);
  if (json.isEmpty()) return false;
  JsonDocument doc;
  if (deserializeJson(doc, json)) {
    LOG_ERR("OTA", "loadPending: bad JSON in %s", OTA_PENDING_FILE);
    return false;
  }
  url = doc["url"] | std::string("");
  version = doc["version"] | std::string("");
  size = doc["size"] | 0;
  return !url.empty() && !version.empty();
}

void OtaUpdater::clearPending() {
  if (Storage.exists(OTA_PENDING_FILE)) Storage.remove(OTA_PENDING_FILE);
}
