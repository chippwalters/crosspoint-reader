#pragma once
// Pure, framework-free helpers for the USB-serial file-transfer protocol
// (SerialFileTransfer). Extracted into a header so they can be host unit-tested
// (test/ft_util) without the Arduino/ESP toolchain. No Arduino String, no SD.
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

namespace FtUtil {

// CRC-32 (zlib/IEEE, polynomial 0xEDB88320). Incremental form: seed with
// 0xFFFFFFFF, call repeatedly over chunks, then XOR the result with 0xFFFFFFFF.
inline uint32_t crc32Update(uint32_t crc, const uint8_t* data, size_t len) {
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return crc;
}

// Whole-buffer convenience: a complete zlib crc32 over [data, data+len).
inline uint32_t crc32(const uint8_t* data, size_t len) {
  return crc32Update(0xFFFFFFFFu, data, len) ^ 0xFFFFFFFFu;
}

// System areas the host must not mutate over serial: CrossPoint config/state
// (/.crosspoint) and the encrypted vault (/Vault). Reads stay allowed; only
// WRITE/DELETE/MKDIR/RMDIR are gated on this. Mirrors the WebDAV protected paths.
inline bool isProtected(const char* path) {
  if (!path) return false;
  auto startsWith = [](const char* s, const char* p) { return std::strncmp(s, p, std::strlen(p)) == 0; };
  return std::strcmp(path, "/.crosspoint") == 0 || startsWith(path, "/.crosspoint/") ||
         std::strcmp(path, "/Vault") == 0 || startsWith(path, "/Vault/");
}

// Parent directory of a path ("/a/b/c.epub" -> "/a/b"). Empty string at root,
// for a top-level path ("/x"), or when there is no slash.
inline std::string parentOf(const char* path) {
  const std::string p(path ? path : "");
  const std::size_t slash = p.find_last_of('/');
  if (slash == std::string::npos || slash == 0) return "";
  return p.substr(0, slash);
}

// Parse an unsigned 32-bit decimal (the size/crc fields of WRITE). Non-numeric
// input yields 0.
inline uint32_t parseU32(const char* s) { return static_cast<uint32_t>(std::strtoul(s ? s : "", nullptr, 10)); }

}  // namespace FtUtil
