---
name: build
description: Build the PaperBit firmware (crosspoint-reader) with PlatformIO — env default (daily) or ble (Notes/BLE keyboard) — following BUILD.md. Use when the user says "build the firmware", "build", "pio build", "make the firmware", or runs /build in the crosspoint-reader repo.
allowed-tools: Read, PowerShell, Glob
---

# Build PaperBit firmware (crosspoint-reader)

Build the PaperBit firmware with PlatformIO. **`BUILD.md` at the repo root is the source of
truth** — read it first and follow it exactly; this skill is the thin orchestrator. If BUILD.md
and this file disagree, BUILD.md wins.

> This is **our** firmware (PaperBit fork of CrossPoint) — never "stock". Commit locally;
> **never `git push` this repo.** Use the **PowerShell** tool; `npm run make` has no place here —
> this is `pio`.

## Procedure

1. **Read `BUILD.md`** (`D:\GitHub\x4\crosspoint-reader\BUILD.md`).

2. **Pick the env** (default `default`; `ble` for the Notes build). If building `ble` to publish,
   note the TEMP `CMD:BLEGATE` harness in `src/main.cpp` should be stripped first (see
   `docs/BLE-KEYBOARD-PLAN.md`).

3. **Bump `[crosspoint] version`** in `platformio.ini` first **if** this build is going to be
   published as an OTA — the device only accepts an update whose leading `major.minor.patch`
   increased (see BUILD.md "OTA gating").

4. **Build** — `PYTHONUTF8` is mandatory (without it the build **false-hangs** on a cp1252 decode
   when output is captured):
   ```powershell
   $env:PYTHONUTF8 = 1
   pio run -d D:\GitHub\x4\crosspoint-reader -e default   # or: -e ble
   ```
   Timeout: 600000 ms.

5. **Confirm artifacts** and report full paths + the git-stamped `version` from the manifest:
   ```powershell
   $e = "default"   # or "ble"
   Get-ChildItem "D:\GitHub\x4\crosspoint-reader\.pio\build\$e\firmware.bin",
                 "D:\GitHub\x4\crosspoint-reader\.pio\build\$e\firmware.manifest.json" |
     Select-Object FullName, Length
   ```

## On failure

- **Hangs with no output** → `PYTHONUTF8` not set; re-run with `$env:PYTHONUTF8 = 1`.
- Otherwise surface the real `pio` error. Never report success on a failed build.

## Next steps

- **Publish the OTA:** run `/publish` (reads PUBLISH.md → `publish-firmware.ps1` →
  `C:\WebDAV\Paperbit`).
- **Flash now:** USB `pio run -e <env> -t upload` under the COM7 lock
  (`D:\GitHub\x4\AGENT-COORD.md`), or Wi-Fi OTA after publishing.
