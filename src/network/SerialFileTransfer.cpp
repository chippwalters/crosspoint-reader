#include "SerialFileTransfer.h"

#include <Arduino.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <Logging.h>
#include <esp_ota_ops.h>
#include <esp_task_wdt.h>

#include <cstdlib>
#include <cstring>
#include <string>

#include "FirmwareFlasher.h"
#include "FtUtil.h"

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

// Pure helpers (crc32, isProtected, parentOf, parseU32) live in FtUtil.h so they
// can be host unit-tested (test/ft_util) without the Arduino toolchain.

// ---- commands ---------------------------------------------------------------

void cmdHello() {
  logSerial.printf("OK:{\"proto\":%u,\"device\":\"%s\",\"fw\":\"%s\",\"chunk\":%u,\"battery\":%u,\"usb\":%s}\n",
                   PROTO_VERSION, gpio.deviceIsX3() ? "X3" : "X4", CROSSPOINT_VERSION, static_cast<unsigned>(CHUNK),
                   static_cast<unsigned>(powerManager.getBatteryPercentage()),
                   gpio.isUsbConnected() ? "true" : "false");
}

// ---- OTA firmware update (wraps the existing brick-safe firmware_flash) ------

// Serial OTA progress, throttled to one line per percent.
int otaLastPct = -1;
void otaProgressCb(size_t written, size_t total, void*) {
  const int pct = total ? static_cast<int>((written * 100) / total) : 0;
  if (pct != otaLastPct) {
    otaLastPct = pct;
    logSerial.printf("OK:OTA_PROGRESS:%u/%u\n", static_cast<unsigned>(written), static_cast<unsigned>(total));
  }
}

// Validate only — never writes flash, never reboots. Zero brick risk.
void cmdOtaDryrun(const String& path) {
  if (path.isEmpty()) {
    err("EINVAL", "empty path");
    return;
  }
  HalFile f;
  if (!Storage.openFileForRead("FT", path.c_str(), f) || !f) {
    err("OTA", "OPEN_FAIL");
    return;
  }
  const size_t size = f.fileSize();
  f.close();
  const esp_partition_t* dest = esp_ota_get_next_update_partition(nullptr);
  if (!dest) {
    err("OTA", "NO_PARTITION");
    return;
  }
  if (size > dest->size) {
    err("OTA", "TOO_LARGE");
    return;
  }
  const auto vr = firmware_flash::validateImageFile(path.c_str(), dest->size);
  if (vr == firmware_flash::Result::OK) {
    logSerial.printf("OK:OTA_DRYRUN_OK:%u\n", static_cast<unsigned>(size));
  } else {
    logSerial.printf("ERR:OTA:%s\n", firmware_flash::resultName(vr));
  }
}

// Real flash: validate -> write the INACTIVE OTA slot -> flip otadata -> reboot.
// The running slot + bootloader + partition table are never touched; any interruption
// leaves the old firmware bootable. Refuses to flash unless on USB power.
void cmdOta(const String& path) {
  if (path.isEmpty()) {
    err("EINVAL", "empty path");
    return;
  }
  if (!gpio.isUsbConnected()) {  // defense-in-depth: never flash on battery
    err("OTA", "NO_USB_POWER");
    return;
  }
  HalFile f;
  if (!Storage.openFileForRead("FT", path.c_str(), f) || !f) {
    err("OTA", "OPEN_FAIL");
    return;
  }
  f.close();

  otaLastPct = -1;
  setSerialLogMuted(true);  // keep log lines out of the OTA_PROGRESS stream
  const auto result = firmware_flash::flashFromSdPath(path.c_str(), otaProgressCb, nullptr);
  setSerialLogMuted(false);

  if (result == firmware_flash::Result::OK) {
    logSerial.printf("OK:OTA_DONE\n");
    logSerial.flush();
    delay(1500);
    ESP.restart();  // boots the freshly written slot
  } else {
    logSerial.printf("ERR:OTA:%s\n", firmware_flash::resultName(result));
  }
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
  if (FtUtil::isProtected(dir.c_str()) && !allowSystem) {
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
  if (FtUtil::isProtected(path.c_str()) && !allowSystem) {
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

void cmdRmdir(const String& dir) {
  if (dir.isEmpty() || dir == "/") {
    err("EINVAL", "refusing to remove root / empty path");
    return;
  }
  if (FtUtil::isProtected(dir.c_str()) && !allowSystem) {
    err("EACCES", "protected path (send CMD:FT:SYS:1 to allow)");
    return;
  }
  if (!Storage.exists(dir.c_str())) {
    err("ENOENT", "no such directory");
    return;
  }
  HalFile f = Storage.open(dir.c_str());
  const bool isDir = f && f.isDirectory();
  if (f) f.close();
  if (!isDir) {
    err("ENOTDIR", "not a directory (use DELETE for files)");
    return;
  }
  if (Storage.removeDir(dir.c_str())) {  // recursive: removes contents then the dir
    ok("RMDIR");
  } else {
    err("EIO", "rmdir failed");
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
  // The global TX timeout is 1 ms (load-bearing for the logging path), which makes
  // write() give up almost immediately when the host drains slowly — the TX ring
  // fills at ~3 KB and the transfer stalls. Raise it for the duration of the stream
  // so write() properly blocks until the ring drains, then restore it.
  logSerial.setTxTimeoutMs(200);
  uint32_t crc = 0xFFFFFFFF;
  size_t sent = 0;
  while (sent < size) {
    const size_t want = (size - sent) < CHUNK ? (size - sent) : CHUNK;
    const int r = f.read(buf, want);
    if (r <= 0) break;
    if (!writeAll(buf, r)) break;  // host stopped draining
    crc = FtUtil::crc32Update(crc, buf, r);
    sent += r;
    esp_task_wdt_reset();
  }
  logSerial.flush();
  logSerial.setTxTimeoutMs(1);  // restore the load-bearing logging timeout
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
  const uint32_t size = FtUtil::parseU32(args.substring(c1 + 1, c2).c_str());
  const uint32_t wantCrc = FtUtil::parseU32(args.substring(c2 + 1).c_str());
  if (path.isEmpty()) {
    err("EINVAL", "empty path");
    return;
  }
  if (FtUtil::isProtected(path.c_str()) && !allowSystem) {
    err("EACCES", "protected path (send CMD:FT:SYS:1 to allow)");
    return;
  }

  const std::string parent = FtUtil::parentOf(path.c_str());
  if (!parent.empty()) Storage.mkdir(parent.c_str());  // create parents as needed

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
    crc = FtUtil::crc32Update(crc, buf, r);
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
  } else if (cmd.startsWith("FT:RMDIR:")) {
    cmdRmdir(cmd.substring(9));
  } else if (cmd.startsWith("FT:READ:")) {
    cmdRead(cmd.substring(8));
  } else if (cmd.startsWith("FT:WRITE:")) {
    cmdWrite(cmd.substring(9));
  } else if (cmd.startsWith("FT:OTA_DRYRUN:")) {
    cmdOtaDryrun(cmd.substring(14));
  } else if (cmd.startsWith("FT:OTA:")) {
    cmdOta(cmd.substring(7));
  } else {
    err("EINVAL", "unknown FT command");
  }
  return true;
}

}  // namespace SerialFileTransfer
