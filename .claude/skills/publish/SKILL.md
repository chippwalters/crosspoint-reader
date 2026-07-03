---
name: publish
description: Publish the PaperBit firmware OTA to the shared Paperbit public folder (C:\WebDAV\Paperbit) via publish-firmware.ps1 — writes firmware.manifest.json (client USB-OTA) + release.json (device Wi-Fi OTA) + the versioned bin. Publishes only when the version changed (unless forced). Follows PUBLISH.md. Does NOT build — run /build first. Use when the user runs /publish or asks to publish/release the firmware.
allowed-tools: Read, PowerShell, Glob
---

# publish — PaperBit firmware OTA

**`D:\GitHub\x4\crosspoint-reader\PUBLISH.md` is the source of truth.** Read it every run and
follow it. This skill **never builds** — it ships what `/build` produced under `.pio\build\<env>\`.
If the bin/manifest are missing, STOP and tell the user to run `/build` first.

> Commit locally; **never `git push` this repo.** The OTA builds from the LOCAL `.pio` output.
> Use the **PowerShell** tool.

Publishes to **`C:\WebDAV\Paperbit\`** (public https://www.widgetgadget.com/cw1/Paperbit/) via
`D:\GitHub\x4\paperbit\webtool\publish-firmware.ps1` — the canonical publisher. It emits
`firmware-<ver>.bin` + `firmware.manifest.json` (client USB-OTA) + `release.json` (device Wi-Fi
OTA) and archives the previous latest.

## Steps

### 1. Read PUBLISH.md
Read the whole `PUBLISH.md` — it carries the env choice, the destination, the "publish only when
changed" rule, and the OTA-gating reminder.

### 2. Choose env + change-check
Default env `default`; use `ble` for the Notes OTA (strip the `CMD:BLEGATE` harness first — see
BUILD.md / `docs/BLE-KEYBOARD-PLAN.md`). Read the built manifest `version` and the live
`release.json` `tag_name`. If **equal**, it's already published — report "already current" and
STOP unless the user forces it. (A same-`major.minor.patch` build won't be accepted by the device
anyway — bump `[crosspoint] version` and rebuild if so.)

### 3. Publish
```powershell
$env:PYTHONUTF8 = 1
pwsh D:\GitHub\x4\paperbit\webtool\publish-firmware.ps1 `
  -BuildDir "D:\GitHub\x4\crosspoint-reader\.pio\build\default" `   # or ...\ble
  -PubDir   "C:\WebDAV\Paperbit"
```

### 4. Verify + report
Report full absolute paths + sizes and the public URLs for `firmware.manifest.json` (client),
`release.json` (device), and `firmware-<ver>.bin`. Heed the script's "version unchanged" WARNING
as a missed version bump.

### 5. (Optional) flash to match
Publishing only makes it installable. To flash now: USB `pio run -e <env> -t upload` under the
**COM7 lock** (`D:\GitHub\x4\AGENT-COORD.md`, `$env:PYTHONUTF8=1`), or Wi-Fi OTA from device
Settings / the desktop client.

Per the fail-loud rule: if any step errors, STOP and report exactly what did/didn't publish.
Never report a publish as complete if a step failed.
