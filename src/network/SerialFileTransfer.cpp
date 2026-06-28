#include "SerialFileTransfer.h"

#include <HalGPIO.h>
#include <HalStorage.h>
#include <Logging.h>
#include <esp_task_wdt.h>

#include <cstdlib>
#include <cstring>

#ifndef CROSSPOINT_VERSION
#define CROSSPOINT_VERSION "unknown"
#endif

namespace SerialFileTransfer {

namespace {

constexpr uint16_t PROTO_VERSION = 1;
constexpr size_t CHUNK = 4096;
constexpr uint32_t READ_TIMEOUT_MS = 5000;
constexpr uint32_t SESSION_TIMEOUT_MS = 60000;  // session auto-expires if host goes silent
constexpr char TMP_SUFFIX[] = ".fttmp";

bool sessionActive = false;
uint32_t lastCmdMs = 0;
// Protected-path writes (/.crosspoint, /Vault) are blocked by default so routine
// file sync can't accidentally clobber settings/vault. A client that *intends* to
// manage system data (Set-OPDS, Clear-cache, Push-vault, Restore) opts in with
// CMD:FT:SYS:1 for the duration; it resets on BEGIN/END.
bool allowSystem = false;

uint8_t buf[CHUNK];

// Incremental CRC-32 (zlib/IEEE, poly 0xEDB88320). Seed/caller convention:
//   uint32_t c = 0xFFFFFFFF; c = crc32_step(c, data, n)...; final = c ^ 0xFFFFFFFF;
uint32_t crc32_step(uint32_t crc, const uint8_t* data, size_t len) {
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return crc;
}

// HWCDC runs with setTxTimeoutMs(1) (load-bearing), so a single write() can send
// fewer bytes than requested when the host drains slowly. Loop until every byte is
// out, yielding so USB can drain — otherwise device->host transfers silently lose data.
bool writeAll(const uint8_t* data, size_t len) {
  size_t off = 0;
  uint32_t lastProgress = millis();
  while (off < len) {
    const size_t w = logSerial.write(data + off, len - off);
    if (w > 0) {
      off += w;
      lastProgress = millis();
    } else if (millis() - lastProgress > 5000) {
      return false;  // host stopped draining; abort
    }
    // Yield EVERY iteration: on the single-core C3 a tight write-spin starves the
    // USB task that drains the TX buffer, so the transfer would deadlock past ~3 KB.
    esp_task_wdt_reset();
    yield();
  }
  return true;
}

void ok(const char* line) { logSerial.printf("OK:%s\n", line); }
void err(const char* code, const char* msg) { logSerial.printf("ERR:%s:%s\n", code, msg); }

uint32_t parseU32(const String& s) { return static_cast<uint32_t>(strtoul(s.c_str(), nullptr, 10)); }

// System areas the host must not mutate over serial: the CrossPoint config/state
// (settings, OPDS, vault.json) and the encrypted vault. Reads are still allowed
// (vault files are encrypted); only WRITE/DELETE/MKDIR are blocked. Mirrors the
// WebDAV "protected paths" rule. Prevents data loss; nothing here is brick-related.
bool isProtected(const String& path) {
  return path == "/.crosspoint" || path.startsWith("/.crosspoint/") || path == "/Vault" ||
         path.startsWith("/Vault/");
}

// Parent directory of a path ("/a/b/c.epub" -> "/a/b"); empty if at root.
String parentOf(const String& path) {
  int slash = path.lastIndexOf('/');
  if (slash <= 0) return "";
  return path.substring(0, slash);
}

// ---- commands ---------------------------------------------------------------

void cmdHello() {
  logSerial.printf("OK:{\"proto\":%u,\"device\":\"%s\",\"fw\":\"%s\",\"chunk\":%u}\n", PROTO_VERSION,
                   gpio.deviceIsX3() ? "X3" : "X4", CROSSPOINT_VERSION, static_cast<unsigned>(CHUNK));
}

void cmdList(const String& dir) {
  const String path = dir.isEmpty() ? String("/") : dir;
  HalFile d = Storage.open(path.c_str());
  if (!d) {
    err("ENOENT", "no such directory");
    return;
  }
  if (!d.isDirectory()) {
    d.close();
    err("ENOTDIR", "not a directory");
    return;
  }
  ok("LIST");
  d.rewindDirectory();
  char name[256];
  for (HalFile e = d.openNextFile(); e; e = d.openNextFile()) {
    e.getName(name, sizeof(name));
    const bool isDir = e.isDirectory();
    const size_t sz = isDir ? 0 : e.size();
    e.close();
    if (name[0] == '\0') continue;
    // tab-delimited: name \t size \t (D|F). Names containing a tab are unsupported.
    logSerial.printf("%s\t%u\t%c\n", name, static_cast<unsigned>(sz), isDir ? 'D' : 'F');
  }
  d.close();
  ok("END");
}

void cmdStat(const String& path) {
  if (!Storage.exists(path.c_str())) {
    logSerial.printf("OK:{\"exists\":false}\n");
    return;
  }
  HalFile f = Storage.open(path.c_str());
  const bool isDir = f && f.isDirectory();
  const size_t sz = (f && !isDir) ? f.size() : 0;
  if (f) f.close();
  logSerial.printf("OK:{\"exists\":true,\"dir\":%s,\"size\":%u}\n", isDir ? "true" : "false",
                   static_cast<unsigned>(sz));
}

void cmdMkdir(const String& dir) {
  if (dir.isEmpty()) {
    err("EINVAL", "empty path");
    return;
  }
  if (isProtected(dir) && !allowSystem) {
    err("EACCES", "protected path (send CMD:FT:SYS:1 to allow)");
    return;
  }
  if (Storage.exists(dir.c_str())) {
    HalFile f = Storage.open(dir.c_str());
    const bool isDir = f && f.isDirectory();
    if (f) f.close();
    if (isDir) {
      ok("MKDIR");  // idempotent: already a directory
    } else {
      err("EEXIST", "path exists and is a file");
    }
    return;
  }
  if (Storage.mkdir(dir.c_str())) {  // pFlag=true -> creates parents
    ok("MKDIR");
  } else {
    err("EIO", "mkdir failed");
  }
}

void cmdDelete(const String& path) {
  if (path.isEmpty()) {
    err("EINVAL", "empty path");
    return;
  }
  if (isProtected(path) && !allowSystem) {
    err("EACCES", "protected path (send CMD:FT:SYS:1 to allow)");
    return;
  }
  if (!Storage.exists(path.c_str())) {
    err("ENOENT", "no such file");
    return;
  }
  HalFile f = Storage.open(path.c_str());
  const bool isDir = f && f.isDirectory();
  if (f) f.close();
  if (isDir) {
    err("EISDIR", "is a directory (v1 deletes files only)");
    return;
  }
  if (Storage.remove(path.c_str())) {
    ok("DELETE");
  } else {
    err("EIO", "delete failed");
  }
}

void cmdRead(const String& path) {
  HalFile f;
  if (!Storage.exists(path.c_str())) {
    err("ENOENT", "no such file");
    return;
  }
  if (!Storage.openFileForRead("FT", path.c_str(), f)) {
    err("EIO", "open failed");
    return;
  }
  if (f.isDirectory()) {
    f.close();
    err("EISDIR", "is a directory");
    return;
  }
  const size_t size = f.size();
  logSerial.printf("OK:READ:%u\n", static_cast<unsigned>(size));

  setSerialLogMuted(true);  // hazard #1: no log lines inside the binary frame
  uint32_t crc = 0xFFFFFFFF;
  size_t sent = 0;
  while (sent < size) {
    const size_t want = (size - sent) < CHUNK ? (size - sent) : CHUNK;
    const int r = f.read(buf, want);
    if (r <= 0) break;
    if (!writeAll(buf, r)) break;  // host stopped draining
    crc = crc32_step(crc, buf, r);
    sent += r;
    esp_task_wdt_reset();
  }
  logSerial.flush();
  f.close();
  crc ^= 0xFFFFFFFF;
  setSerialLogMuted(false);

  if (sent == size) {
    logSerial.printf("OK:END:%lu\n", static_cast<unsigned long>(crc));
  } else {
    err("EIO", "short read");
  }
}

void cmdWrite(const String& args) {
  // args = "<path>:<size>:<crc>"  (size and crc are the last two ':'-separated fields)
  const int c2 = args.lastIndexOf(':');
  const int c1 = c2 > 0 ? args.lastIndexOf(':', c2 - 1) : -1;
  if (c1 < 0 || c2 < 0) {
    err("EINVAL", "usage FT:WRITE:<path>:<size>:<crc>");
    return;
  }
  const String path = args.substring(0, c1);
  const uint32_t size = parseU32(args.substring(c1 + 1, c2));
  const uint32_t wantCrc = parseU32(args.substring(c2 + 1));
  if (path.isEmpty()) {
    err("EINVAL", "empty path");
    return;
  }
  if (isProtected(path) && !allowSystem) {
    err("EACCES", "protected path (send CMD:FT:SYS:1 to allow)");
    return;
  }

  const String parent = parentOf(path);
  if (!parent.isEmpty()) Storage.mkdir(parent.c_str());  // create parents as needed

  const String tmp = path + TMP_SUFFIX;
  Storage.remove(tmp.c_str());
  HalFile f;
  if (!Storage.openFileForWrite("FT", tmp.c_str(), f)) {
    err("EIO", "open temp failed");
    return;
  }

  logSerial.printf("OK:WRITE_READY:%u\n", static_cast<unsigned>(CHUNK));
  logSerial.flush();

  setSerialLogMuted(true);
  logSerial.setTimeout(READ_TIMEOUT_MS);
  uint32_t got = 0, crc = 0xFFFFFFFF;
  bool timedOut = false, sdFull = false;
  while (got < size) {
    const uint32_t rem = size - got;
    const size_t want = rem < CHUNK ? rem : CHUNK;
    const size_t r = logSerial.readBytes(buf, want);
    if (r == 0) {
      timedOut = true;
      break;
    }
    if (f.write(buf, r) != r) {  // short write => card full / SD write error
      sdFull = true;
      break;
    }
    crc = crc32_step(crc, buf, r);
    got += r;
    esp_task_wdt_reset();
    logSerial.printf("OK:ACK:%u\n", static_cast<unsigned>(got));  // flow-control credit
  }
  f.flush();
  f.close();
  crc ^= 0xFFFFFFFF;
  setSerialLogMuted(false);

  if (sdFull) {
    Storage.remove(tmp.c_str());
    err("ENOSPC", "SD write failed (card full?)");
    return;
  }
  if (timedOut || got != size) {
    Storage.remove(tmp.c_str());
    err("ETIMEDOUT", "incomplete upload");
    return;
  }
  if (crc != wantCrc) {
    Storage.remove(tmp.c_str());
    err("ECRC", "checksum mismatch");
    return;
  }
  // Atomic publish: drop any existing final, then rename temp into place.
  Storage.remove(path.c_str());
  if (Storage.rename(tmp.c_str(), path.c_str())) {
    logSerial.printf("OK:WRITE_DONE:%u\n", static_cast<unsigned>(got));
  } else {
    Storage.remove(tmp.c_str());
    err("EIO", "rename failed");
  }
}

}  // namespace

bool keepAwake() { return sessionActive && (millis() - lastCmdMs < SESSION_TIMEOUT_MS); }

bool handle(const String& cmd) {
  if (!cmd.startsWith("FT:")) return false;
  lastCmdMs = millis();

  if (cmd == "FT:HELLO") {
    cmdHello();
  } else if (cmd == "FT:BEGIN") {
    sessionActive = true;
    allowSystem = false;
    ok("session");
  } else if (cmd == "FT:END") {
    sessionActive = false;
    allowSystem = false;
    ok("bye");
  } else if (cmd == "FT:PING") {
    ok("pong");
  } else if (cmd == "FT:SYS:1") {
    allowSystem = true;  // allow writes/deletes under /.crosspoint and /Vault this session
    ok("sys-on");
  } else if (cmd == "FT:SYS:0") {
    allowSystem = false;
    ok("sys-off");
  } else if (cmd.startsWith("FT:LIST:")) {
    cmdList(cmd.substring(8));
  } else if (cmd.startsWith("FT:STAT:")) {
    cmdStat(cmd.substring(8));
  } else if (cmd.startsWith("FT:MKDIR:")) {
    cmdMkdir(cmd.substring(9));
  } else if (cmd.startsWith("FT:DELETE:")) {
    cmdDelete(cmd.substring(10));
  } else if (cmd.startsWith("FT:READ:")) {
    cmdRead(cmd.substring(8));
  } else if (cmd.startsWith("FT:WRITE:")) {
    cmdWrite(cmd.substring(9));
  } else {
    err("EINVAL", "unknown FT command");
  }
  return true;
}

}  // namespace SerialFileTransfer
