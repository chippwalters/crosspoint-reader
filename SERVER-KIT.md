# PaperBit Server Kit — self-hosting guide

Everything device-facing in PaperBit is deliberately dumb on the server side: **static files in
folders over plain HTTP**. Any web server you already run — Apache, nginx, Caddy, a NAS — can host
both the content the device fetches and the firmware updates it installs. This guide covers the
folder layout, the file formats, and the one rule you must not break.

## The one rule

> **The device speaks plain `http://` only. Your server MUST serve the Fetch folders and the OTA
> feed over HTTP without redirecting to HTTPS.**

An `http→https` redirect (or an HSTS-style forced upgrade) breaks the device: the ESP32-C3 cannot
run an in-session TLS handshake with this firmware's memory layout (mbedTLS needs 16 KB + 16 KB
contiguous buffers; the device has ~30–45 KB largest free block mid-session). Serve the same
paths over HTTPS too if you like — browsers and the desktop app will use it — but the plain-HTTP
path must work end-to-end. Threat model and mitigations are covered in the firmware README's
security-posture section.

## Folder layout

One release folder plus any number of Fetch folders:

```
webroot/
├── paperbit/                      # the release folder (OTA + downloads)
│   ├── release.json               # device Wi-Fi OTA feed
│   ├── firmware-<version>.bin     # exactly one "latest" versioned binary
│   ├── firmware.manifest.json     # desktop client's USB-install entry point
│   ├── archive/<old-version>/     # prior releases (optional)
│   └── docs/                      # published manuals as raw .md (optional —
│                                  #   raw Markdown is directly Fetch-able)
├── fetch/                         # a Paperbit Fetch folder (name it anything)
│   ├── groceries.md
│   ├── daily-digest.md
│   └── index.json                 # OPTIONAL — curated names/order
└── capture/                       # a Fetch folder with the write API (optional)
    ├── write.php
    └── *.md
```

## Fetch folders

A Fetch folder is just a directory of documents the device lists and downloads. Supported file
types: `.md`, `.epub`, `.txt`, `.xtc`.

**Directory listing (zero config on Apache):** the device parses a standard autoindex page.

```apache
# .htaccess in the Fetch folder
Options +Indexes
```

Any server whose index page emits plain `<a href>` links to the files works; the device skips
parent/sort/subdirectory links and de-duplicates icon+text double links.

**`index.json` (optional):** put one in the folder to control display names and order instead of
relying on the listing:

```json
[
  { "name": "Groceries",     "file": "groceries.md" },
  { "name": "Daily digest",  "file": "daily-digest.md" }
]
```

- `name` — what the device shows in the list.
- `file` — the filename to download; defaults to `<name>.md` if omitted.
- Unknown fields are ignored, so you can carry extra metadata (`mtime`, etc.).

Point the device at the folder URL (e.g. `http://your-server/fetch/`) via the desktop app, the
device's web Settings page, or on-device (Paperbit Fetch → Set source URL).

## The OTA release feed — `release.json`

The device's **Settings → Check for updates** fetches `release.json` from your release folder.
The shape is GitHub-releases-compatible:

```json
{
  "tag_name": "1.4.16-ble-paperbit-00be2371",
  "name": "1.4.16-ble-paperbit-00be2371",
  "assets": [
    {
      "name": "firmware.bin",
      "browser_download_url": "http://your-server/paperbit/firmware-1.4.16-ble-paperbit-00be2371.bin",
      "size": 5551840
    }
  ]
}
```

- The device looks for the asset **named exactly `firmware.bin`** and downloads its
  `browser_download_url` — which must be a **plain `http://` URL** (the one rule, again).
- **Version gate:** the device only offers an update when the leading **`major.minor.patch`** of
  `tag_name` is newer than what's running. Suffixes (build flavor, branch, SHA) are ignored by
  the comparison — every release you want devices to install needs a version bump.
- The asset's **`sha256` field** (lowercase hex of the whole `.bin`; supported since firmware
  1.4.17): the device hashes the downloaded image and refuses to flash on a mismatch. Optional —
  when omitted, the device still runs its image-internal validation — but always include it; the
  publish script emits it automatically. Image validation + A/B slots protect regardless.

`firmware.manifest.json` sits beside it and is what the **desktop client's** USB installer reads:
`{ target, product, device, branch, gitHash, version, file, sha256, size }` — the client verifies
magic bytes, sha256, size, and target device before flashing.

## The publish script

You don't have to maintain this layout by hand. The repo's `publish-firmware.ps1` takes a
PlatformIO build directory and does the bookkeeping in one shot:

- archives the previous latest to `archive/<old-version>/`,
- copies the single versioned `firmware-<version>.bin` + its `firmware.manifest.json`,
- regenerates `release.json` with plain-`http://` download URLs,
- warns if the version didn't change (i.e., you forgot to bump — devices would ignore it).

If you host elsewhere, replicate those four behaviors and any static host works.

## The capture write API — `write.php` (optional)

If you want phones (the PaperBit Capture app) or scripts to *add* documents to a Fetch folder,
drop `write.php` into it. It's the **Write API v1** — a single endpoint, no framework:

```
POST <folder>/write.php     Content-Type: application/json

{ "action": "save",   "name": "My Note.md", "content": "# My Note\n\n..." }
{ "action": "delete", "name": "My Note.md" }
```

Responses: `200 {"ok":true,"file":"My Note.md"}`, or `{"ok":false,"error":"..."}` with `400`
(bad name / too large / bad json / unknown action), `404` (delete of a missing file), or `500`.

Guards enforced server-side even though v1 has **no auth**:

- `name` must match `^[A-Za-z0-9 _.\-]{1,100}\.md$` — basename only, no `/`, `\`, or `..`
  (realpath-parent check on delete).
- `content` max **512 KB**, `.md` only.
- Saves are **atomic** (temp file + rename) — a reader never sees a half-written file.
- No index file is touched; the device's listing reflects the folder contents on its next read.
- CORS: permissive `Access-Control-Allow-Origin: *`, `OPTIONS` handled.

**No auth means anyone who can reach the URL can write `.md` files to that folder.** Host it on a
LAN, behind your reverse proxy's access rules, or accept the exposure knowingly — and keep
`write.php` only in folders that need it. Adding a token is the planned v1.1.

## Checklist

- [ ] Release folder serves `release.json` + the versioned `.bin` over **plain HTTP, no redirect**.
- [ ] Fetch folder serves either an autoindex listing or an `index.json`, over **plain HTTP, no redirect**.
- [ ] `browser_download_url` in `release.json` is `http://` and resolves.
- [ ] Every published release bumps `major.minor.patch`.
- [ ] `write.php` only where you want unauthenticated writes.
