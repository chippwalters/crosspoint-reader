# Contributing to PaperBit

Thanks for your interest. Before you write code or file an issue, please read this — it will save
us both time.

## Set expectations: this is a hobby project

PaperBit is maintained in spare time by a very small number of people. Issues and PRs are read,
but responses may take days or weeks, and not every good idea will be merged — the project
optimizes for a small, understandable codebase over feature count. If you need a change urgently,
the MIT license means you can fork.

## Filing a good issue

E-ink + BLE + Wi-Fi on a 380 KB microcontroller means most bugs are environmental. An issue we
can act on includes:

1. **Firmware version** — Settings → System → About on the device (the full string, e.g.
   `1.4.16-ble-...`), and which device (X4 or X3).
2. **Your network, if the bug involves Wi-Fi/Fetch/OTA** — 2.4 GHz or dual-band SSID (the device
   is 2.4 GHz only), mesh or single AP, guest network, whether the AP has client isolation, and
   whether anything on the path redirects HTTP to HTTPS (that breaks the device by design — see
   the security section of the README).
3. **Peripherals paired** — BLE keyboard make/model if Notes is involved. Remember: BLE and Wi-Fi
   never run at the same time on this device; that's a hardware constraint, not a bug.
4. **When it last worked** — did this ever work? On which firmware version? What changed between
   then and now (router, keyboard, server, update)?
5. **What you saw on-screen** — PaperBit fails loud; the exact error text ("Can't reach the
   source", "Source not found (404)", "Update FAILED", …) narrows things down fast.

Please don't file "switch device traffic to HTTPS" issues without engaging with the heap math in
the README's security-posture section. It has been measured on hardware.

## Pull requests

- **Open an issue first** for anything non-trivial, so the approach can be agreed before you
  invest time.
- **Firmware PRs require hardware.** If you're touching device code, you must have flashed and
  exercised your change on a real X4 or X3. "It compiles" is not verification — most of this
  firmware's hard-won behavior (radio handoff, refresh timing, heap discipline) only shows up on
  glass. Say in the PR what you tested and on which device.
- **Respect the memory governor.** The working heap is ~128 KB with ~30–45 KB largest free block.
  No DOM, no large intermediate strings, stream everything, and prefer failing loud (a visible
  on-screen marker) over degrading silently.
- **Match the surrounding code.** Each repo has its own established style (firmware C++, desktop
  Electron/React, mobile Flutter). Follow what's already there; don't introduce a new formatter,
  lint config, or directory convention in a feature PR.
- **No drive-by refactors.** Keep diffs scoped to the change you're making. A PR that fixes one
  bug and reformats twelve files will be asked to shrink; renames, "cleanup", and dependency bumps
  belong in their own discussed PRs.
- **Keep the OTA path sacred.** Anything touching the updater, partition handling, or image
  validation gets extra scrutiny — that's the anti-brick machinery.

## Contributions that don't need hardware

- Documentation fixes and clarifications.
- The server kit (PHP/tooling) and desktop/mobile apps — testable without a device (the desktop
  app has a `mock` transport).
- Markdown parser test cases: real-world `.md` files that render wrong are genuinely useful —
  attach the file to an issue.

Known open areas on the firmware side, if you want something meaty: heading **size** hierarchy
(needs font-size/multi-face engine work), a real monospace face for code, and interactive
task-list checkboxes.

## Licensing of contributions

By submitting a PR you agree your contribution is licensed under the repo's license (MIT for the
code repos; CC BY 4.0 for the design docs).
