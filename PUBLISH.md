# Publishing PaperBit firmware (OTA)

<!-- publish_timeout: 300000 -->

**Source of truth for publishing the PaperBit firmware OTA.** The `/publish` skill
(`.claude/skills/publish/`) reads it every run. This skill **never builds** — it publishes what
`/build` (BUILD.md) already produced under `.pio\build\<env>\`. If the bin/manifest are missing,
STOP and run `/build` first.

> Commit locally; **never `git push` this repo.** The OTA is built from the LOCAL `.pio` output,
> so a push is never required.

## Destination — the shared Paperbit public folder

Publishes to **`C:\WebDAV\Paperbit\`** → **`https://www.widgetgadget.com/cw1/Paperbit/`** via the
canonical script **`D:\GitHub\x4\paperbit\webtool\publish-firmware.ps1`**. It writes three things
and archives the previous "latest":

| File | Consumer |
|------|----------|
| `firmware-<version>.bin` | the one versioned binary |
| `firmware.manifest.json` | desktop **client** USB-OTA entry point |
| `release.json` | **device** Wi-Fi OTA feed (Settings → Check for updates → `esp_https_ota`) |

(The client ZIP also lives in this folder under `Paperbit-win32-x64-*.zip` — the firmware script
only touches `firmware*.bin` / `*.json`, so the two never collide.)

## Publish only when changed

The published `release.json` `tag_name` is the currently-live firmware version. If the freshly
built `firmware.manifest.json` `version` **equals** that `tag_name`, it's already published —
report "already current" and STOP, unless the user forces a re-publish. (Remember: to make a
new OTA the device will actually *accept*, the leading `major.minor.patch` must have increased —
bump `[crosspoint] version` in `platformio.ini` before building. See BUILD.md OTA gating.)

## Steps (use the PowerShell tool, Windows host)

### 1. Choose the env + verify the build exists

Default env is `default`. For the **Notes** OTA use `ble` (strip the `CMD:BLEGATE` harness first —
see BUILD.md).

```powershell
$env_ = "default"   # or "ble"
$buildDir = "D:\GitHub\x4\crosspoint-reader\.pio\build\$env_"
if (-not (Test-Path "$buildDir\firmware.bin") -or -not (Test-Path "$buildDir\firmware.manifest.json")) {
  throw "No build in $buildDir - run /build (BUILD.md) first."
}
$builtVer = (Get-Content "$buildDir\firmware.manifest.json" -Raw | ConvertFrom-Json).version
$liveVer  = if (Test-Path "C:\WebDAV\Paperbit\release.json") {
  (Get-Content "C:\WebDAV\Paperbit\release.json" -Raw | ConvertFrom-Json).tag_name } else { "" }
Write-Output "built=$builtVer live=$liveVer"
# If $builtVer -eq $liveVer -> already published; STOP unless forced.
```

### 2. Publish (build + manifest + release feed, archives the old latest)

```powershell
$env:PYTHONUTF8 = 1
pwsh D:\GitHub\x4\paperbit\webtool\publish-firmware.ps1 -BuildDir $buildDir -PubDir "C:\WebDAV\Paperbit"
```

The script archives the previous latest to `archive\<oldVersion>\`, copies
`firmware-<version>.bin` + `firmware.manifest.json`, and regenerates `release.json`. It WARNS
(does not fail) if the version is unchanged — heed that as "you forgot to bump".

### 3. Verify + report (full absolute paths, public URLs)

```powershell
Get-ChildItem "C:\WebDAV\Paperbit\firmware-*.bin","C:\WebDAV\Paperbit\firmware.manifest.json","C:\WebDAV\Paperbit\release.json" |
  Select-Object FullName, Length, LastWriteTime
```

Report:
- Client USB-OTA entry: `https://www.widgetgadget.com/cw1/Paperbit/firmware.manifest.json`
- Device Wi-Fi-OTA feed: `https://www.widgetgadget.com/cw1/Paperbit/release.json`
- Binary: `https://www.widgetgadget.com/cw1/Paperbit/firmware-<version>.bin`

### 4. (Optional) flash the device so it matches the published OTA

Publishing only makes the firmware *installable*. To put it on the device now, flash via USB
(under the **COM7 lock** in `D:\GitHub\x4\AGENT-COORD.md`, `$env:PYTHONUTF8=1`,
`pio run -e <env> -t upload`) or trigger Wi-Fi OTA from device Settings / the desktop client.

Per the fail-loud rule: if any step errors, STOP and report exactly what did/didn't publish.
