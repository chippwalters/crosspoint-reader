# Notices & Third-Party Attributions

**PaperBit firmware** — a fork of CrossPoint Reader for the Xteink X4/X3, by Chipp Walters.
PaperBit's modifications and additions are released under the same **MIT License** as the
upstream project (see `LICENSE`).

## Upstream & derived work

| Component | Origin | License |
|---|---|---|
| **CrossPoint Reader** (the base of this entire firmware) | Dave Allie — [crosspoint-reader/crosspoint-reader](https://github.com/crosspoint-reader/crosspoint-reader) | MIT © 2025 Dave Allie |
| **Markdown parser** (`lib/Epub/Epub/markdown/md_parser.*`) | Derived from [papyrix-reader](https://github.com/bigbag/papyrix-reader)'s `lib/Markdown` (same upstream author) | MIT © Dave Allie |
| **open-x4-sdk** (HAL/display/input submodule) | CrossPoint community SDK (forked: `chippwalters/community-sdk@paperbit`) | MIT |

## Libraries

| Library | Use | License |
|---|---|---|
| [NimBLE-Arduino](https://github.com/h2zero/NimBLE-Arduino) (h2zero) | BLE HID host for the Notes keyboard | Apache-2.0 |
| ArduinoJson (bblanchon) | settings / feeds | MIT |
| PNGdec, JPEGDEC (bitbank2) | image decoding | Apache-2.0 |
| QRCode (ricmoo) | Wi-Fi setup QR | MIT |
| arduinoWebSockets (links2004) | Calibre wireless | LGPL-2.1 |
| ESP-IDF / arduino-esp32 (Espressif, via pioarduino) | platform | Apache-2.0 |

## Acknowledgements

- **[MicroSlate](https://github.com/Josh-writes/microslate-firmware)** (MIT) — the reference that
  proved BLE keyboard input on this exact hardware and informed the NimBLE central bring-up.
- The CrossPoint community, whose reader this project stands on.
