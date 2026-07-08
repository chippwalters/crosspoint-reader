# Flashing PaperBit

How to get PaperBit firmware onto an Xteink X4 or X3, and how to get back to stock CrossPoint if
you want to.

## Read this first — warranty and risk

Flashing third-party firmware is at your own risk and may void your warranty. That said, the
realistic risk here is low by design:

- PaperBit (like upstream CrossPoint) uses **dual A/B firmware slots**. The OTA and SD-card
  update paths validate the image and write only the **inactive** slot — the bootloader and
  partition table are never touched, so a failed or interrupted update still boots the previous
  firmware.
- The ESP32-C3 has a **ROM USB download mode** that no firmware can break: even a device with
  completely broken firmware can be re-flashed with `esptool` over the same USB cable.

The only genuinely careful moment is a full `esptool` flash (it writes flash directly, bypassing
the validation layers). Don't unplug mid-write, and you'll be fine.

## What you need

- An Xteink **X4** or **X3**.
- A **USB-C cable that carries data** (many bundled cables are charge-only — if no serial port
  appears, try another cable first).
- A computer. The device shows up as a plain USB serial port (native USB-Serial/JTAG — no driver
  install needed on current Windows/macOS/Linux).
- A PaperBit `firmware-<version>.bin` from the releases page.

Note: if the device is in **deep sleep, the USB port disappears entirely**. Press a button to
wake it before flashing.

## Path 1 — SD-card update (easiest, if you already run CrossPoint or PaperBit)

1. Copy the firmware binary to the SD card, renamed to `firmware.bin`.
2. On the device: **Settings → System → SD Firmware Update**.
3. The device validates the image, writes the inactive slot, and reboots into PaperBit.

This uses the same brick-safe path as OTA. It's the recommended route for a device already on
CrossPoint.

## Path 2 — esptool over USB (from-scratch / any state)

Install esptool and find your port:

```powershell
# Windows
pip install esptool
# port = the "USB Serial Device (COMx)" in Device Manager, e.g. COM7
```

```bash
# macOS / Linux
pip install esptool
# port = /dev/ttyACM0 (Linux) or /dev/cu.usbmodem* (macOS)
```

Flash the app image (it lives at offset `0x10000`), then clear the OTA-slot selector so the
device boots the slot you just wrote:

```powershell
esptool --chip esp32c3 --port COM7 --baud 921600 write_flash 0x10000 firmware-<version>.bin
esptool --chip esp32c3 --port COM7 erase_region 0xe000 0x2000
```

```bash
esptool --chip esp32c3 --port /dev/ttyACM0 --baud 921600 write_flash 0x10000 firmware-<version>.bin
esptool --chip esp32c3 --port /dev/ttyACM0 erase_region 0xe000 0x2000
```

The `erase_region` step wipes the `otadata` record (at `0xe000`): a device that has taken OTA
updates may be running from the *second* slot, and without this step it would keep booting the
old firmware from there. With `otadata` erased, the bootloader falls back to the first slot —
the one you just flashed.

If esptool can't connect, the ROM download mode is the fallback: hold the device's boot/download
control while plugging in USB, then retry. (Check your device's documentation for the exact
button — entering download mode manually is rarely needed since the running firmware accepts the
reset sequence esptool sends.)

## Path 3 — building and flashing from source

With PlatformIO (see the README's build section — remember `PYTHONUTF8=1` on Windows):

```powershell
$env:PYTHONUTF8 = '1'
pio run -e ble -t upload --upload-port COM7
```

PlatformIO handles offsets and resets itself.

## After the first flash

Wireless updates take over from here: **Settings → Check for updates** fetches new releases over
Wi-Fi and installs them with on-screen progress. You shouldn't need a cable again unless you want
one.

## Going back to stock CrossPoint

PaperBit is a fork of [CrossPoint Reader](https://crosspointreader.com) and keeps its partition
layout, so returning to stock is straightforward:

- **Web flasher:** the CrossPoint web installer at
  [crosspointreader.com](https://crosspointreader.com/#flash-tools) flashes stock CrossPoint from
  a Chromium browser over the same USB cable — the simplest full restore, and also the recovery
  tool of last resort.
- **esptool / SD:** flash an official CrossPoint release the same ways described above.

Your books and files live on the SD card and are untouched by firmware changes either direction.
(PaperBit-specific data — Vault files, Notes, Fetch downloads — will simply be ignored by stock
CrossPoint.)
