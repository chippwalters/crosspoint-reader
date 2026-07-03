# Building PaperBit firmware (crosspoint-reader)

<!-- build_timeout: 600000 -->

**Source of truth for building the PaperBit firmware.** The `/build` skill
(`.claude/skills/build/`) is a thin orchestrator over this file — if they disagree, this
file wins.

> This is **our** firmware — the PaperBit fork of CrossPoint. Never call it "stock".
> Commit locally; **never `git push` this repo** (see the repo rule).

## Build environments (PlatformIO)

| Env | Flag / purpose | Version stamp |
|-----|----------------|---------------|
| `default` | daily driver (serial log, debug) | `{base}-dev-{branch}-{sha}` (git-stamped) |
| `ble` | **Notes / BLE keyboard** (`-DENABLE_BLE_KEYBOARD`) — the Notes-enabled build | `{base}-ble-{branch}-{sha}` (git-stamped) |
| `slim` | serial log off, smallest | `{base}-slim` (static) |

`{base}` = `[crosspoint] version` in `platformio.ini` (currently **1.4.13**). The `default` and
`ble` versions are git-SHA-stamped by `scripts/git_branch.py` (`GIT_STAMPED_ENVS`). Release
envs (`gh_release*`) set their own version and are not used for our OTA.

> **OTA gating (critical):** the device's Wi-Fi OTA (`OtaUpdater::isUpdateNewer`) only offers an
> update when the leading **`major.minor.patch`** increases. A new SHA alone is NOT enough — to
> ship any OTA you must **bump `[crosspoint] version`** first, else the device sees "up to date".

## Procedure

1. **Pick the env.** Default is `default`. For the **Notes** build use `ble`.
   > ⚠️ The `ble` env still carries the TEMP `CMD:BLEGATE` §6 dev harness in `src/main.cpp`
   > (+ the TEMP `esp_heap_caps.h` include). **Strip it before publishing `ble` as the official
   > OTA** — see `docs/BLE-KEYBOARD-PLAN.md`.

2. **Bump the base version if publishing** (`platformio.ini` → `[crosspoint] version`), so the
   new build out-ranks whatever is on the device.

3. **Build** (UTF-8 env is mandatory — without `PYTHONUTF8=1` the build **false-hangs** on a
   cp1252 decode when output is captured):
   ```powershell
   $env:PYTHONUTF8 = 1
   pio run -d D:\GitHub\x4\crosspoint-reader -e default   # or: -e ble
   ```

4. **Confirm the artifacts** (the `gen_manifest.py` post-script emits the manifest next to the bin):
   ```powershell
   $env = "default"   # or "ble"
   Get-ChildItem "D:\GitHub\x4\crosspoint-reader\.pio\build\$env\firmware.bin",
                 "D:\GitHub\x4\crosspoint-reader\.pio\build\$env\firmware.manifest.json" |
     Select-Object FullName, Length
   ```
   The manifest's `version` is the git-stamped string — report it.

## Flashing (separate from build/publish)

- **USB flash** (needs the COM7 device lock — serialize via `D:\GitHub\x4\AGENT-COORD.md`;
  set `$env:PYTHONUTF8=1`): `pio run -e <env> -t upload`.
- **Wireless** happens after `/publish` (device Settings → Check for updates, or the desktop
  client OTA). See PUBLISH.md.

## On failure

- **Build appears to hang with no output** → `PYTHONUTF8` wasn't set; re-run with
  `$env:PYTHONUTF8 = 1`.
- **Upload can't open COM7** → another actor holds the port; claim the lock in
  `D:\GitHub\x4\AGENT-COORD.md`, or wait out the 60 s FT session timeout.

Never report success on a failed build — surface the real `pio` error.
