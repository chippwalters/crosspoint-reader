# CLAUDE.md — PaperBit firmware (crosspoint-reader, branch `paperbit`)

The **PaperBit firmware** for the Xteink **X4/X3** e-ink readers — our fork of CrossPoint Reader
(never call it "stock"). ESP32-C3, PlatformIO/Arduino. This repo is the device-side engine: EPUB
layout, native Markdown reader, Paperbit Fetch, Notes/BLE keyboard, wireless OTA, USB-serial FT.

## Hard rules (read first)

1. **NEVER `git push` — any remote.** Commit **locally only**. The OTA publishes from the local
   `.pio` output; a push is never required.
2. **`$env:PYTHONUTF8='1'` before every `pio` command** on Windows, or it false-hangs (cp1252).
3. **COM7 is exclusive & lock-gated** — claim `D:\GitHub\x4\AGENT-COORD.md` before flashing/serial
   (the Paperbit desktop app also holds the port; stop it first).
4. **The ESP32-C3 is the memory governor**: ~191 KB total heap on the ble build, two static 52 KB
   framebuffers already spent. Stream everything; no DOM; measure before building. **In-session TLS
   is infeasible** (mbedTLS needs 16K+16K contiguous buffers vs ~28–45K largest free block) — device
   HTTP fetches use **plain `http://`** by design.
5. **Bump `[crosspoint] version`** (platformio.ini) for every published OTA — the device update gate
   compares leading `major.minor.patch` only.
6. **Fail loud, never fake** — visible on-device degraded markers, never silent blanks.

## Build / flash / publish

`BUILD.md` and `PUBLISH.md` (repo root) are the sources of truth; `/build` and `/publish` project
skills orchestrate them.

```powershell
$env:PYTHONUTF8='1'
pio run -e default            # daily build  -> {base}-dev-{branch}-{sha}
pio run -e ble                # Notes/BLE keyboard build (THE shipped flavor) -> {base}-ble-{branch}-{sha}
pio run -e ble -t upload      # USB flash over COM7 (under the lock)
pwsh D:\GitHub\x4\paperbit\webtool\publish-firmware.ps1 -BuildDir .\.pio\build\ble
```

Versions are git-stamped by `scripts/git_branch.py` (`GIT_STAMPED_ENVS`); `scripts/gen_manifest.py`
mirrors the channel map into `firmware.manifest.json`. Host unit tests: `pio run -t unit-tests`
(needs Visual Studio; WSL `g++` works for quick standalone harnesses).

## The OTA design (don't regress it)

**Reboot-to-install, all plain HTTP** (`src/network/OtaUpdater.*`, `src/main.cpp
maybeRunPendingOta`): Settings check fetches `http://…/Paperbit/release.json` in-session → on
confirm, persist `/.crosspoint/ota_pending.json` + restart → early boot (pristine heap) downloads
the image to SD via `HttpDownloader::downloadToFile` → Wi-Fi off → flash via
`firmware_flash::flashFromSdPath` (validates image; A/B slots are the anti-brick safety) → reboot.
`esp_https_ota` is deliberately NOT used. One-shot pending file; fails loud on-screen and boots
normally on failure.

## Wi-Fi / BLE gotchas (all field-diagnosed on hardware)

- **`WiFi.setSleep(false)` after every connect** — done at the `WL_CONNECTED` chokepoint in
  `WifiSelectionActivity`; any new Wi-Fi path that bypasses it must do this itself (modem power-save
  corrupts traffic on some APs).
- **Never set `skip_cert_common_name_check`** — esp-tls implements it as `set_hostname(NULL)`, which
  disables SNI; a name-based vhost then serves the wrong cert.
- **BLE and Wi-Fi never run together** (single radio): `BleKeyboardManager::deinit(false)` fully
  tears down NimBLE on activity exit (keeps bonds). BLE init requires full CPU speed — hold
  `HalPowerManager::Lock` (the C3 controller deadlocks at the 10 MHz idle throttle).
- **Never launch activities from the serial command handler** in `main.cpp` — it deadlocks the main
  loop. Serial diag commands must stay self-contained (RenderLock + power lock, do work, return).
- The home AP has **client isolation**: the device reaches the internet but NOT LAN hosts.

## Other firmware gotchas

- **Panel rotates bitmaps 90° CW** → author icons pre-rotated 90° CCW (`src/components/icons/*.h`,
  wired in `LyraTheme::iconForName`). Live text is drawn upright.
- **Never bypass the `HalStorage` SD mutex** (use `Storage`, not raw SdFat / `SdMan`).
- Wi-Fi activities `silentRestart()` on exit to clear LWIP/mbedTLS heap fragmentation.
- `ENABLE_BLE_KEYBOARD` code lives in guarded TUs — the `default`/`slim` builds must stay
  byte-unaffected by Notes/BLE changes.
- Serial log + FT share the USB CDC; `CMD:SCREENSHOT` mutes logging while streaming.

## Map / canonical docs

- Architecture (canonical, incl. §11 host↔device): `D:\GitHub\x4\paperbit\ARCHITECTURE.md`
- FT wire protocol: `D:\GitHub\x4\paperbit\docs\usb-serial-file-transfer.md`
- Design system / PRD: `D:\GitHub\paperbit-os\docs\` (SPEC.md, PRD.md, MEMORY-BUDGET.md)
- Host-side diag tools (run app-stopped): `D:\GitHub\x4-client\tools\_pb*.cjs` (log tap,
  screenshot→PNG, wifi/fetch-url seeding, OTA driver)
- Key subsystems: `src/activities/` (Activity stack; Home/reader/Fetch/Notes/Settings),
  `lib/Epub` + `src/Epub/markdown/` (layout + native MD), `src/network/` (FT, HttpDownloader,
  OtaUpdater, web server), `src/ble/` (NimBLE keyboard), `lib/hal/` (display/storage/power).
