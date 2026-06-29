"""
PlatformIO post-build script: emit firmware.manifest.json beside firmware.bin.

This is the trust-anchor manifest for host-side OTA verification (the device does
NOT enforce secure boot / signatures), used by the Paperbit firmware-install flow.
The host verifies the uploaded image against this before flashing:
  - target == "esp32c3"  (chip)
  - device  matches the connected device (HELLO.device, X3 or X4 — one universal binary)
  - sha256 / size match the uploaded firmware.bin
  - after flashing, HELLO.fw contains gitHash

Version fields mirror CROSSPOINT_VERSION (see git_branch.py).
"""

import configparser
import hashlib
import json
import os
import subprocess

Import("env")  # noqa: F821  -- provided by PlatformIO


def _git(project_dir, args):
    try:
        return subprocess.check_output(
            ["git", *args], text=True, stderr=subprocess.DEVNULL, cwd=project_dir
        ).strip()
    except Exception:  # pylint: disable=broad-exception-caught
        return "unknown"


def _base_version(project_dir):
    cfg = configparser.ConfigParser()
    try:
        cfg.read(os.path.join(project_dir, "platformio.ini"))
        return cfg.get("crosspoint", "version")
    except Exception:  # pylint: disable=broad-exception-caught
        return "0.0.0"


def after_bin(source, target, env):  # noqa: ARG001
    build_dir = env.subst("$BUILD_DIR")
    bin_path = os.path.join(build_dir, "firmware.bin")
    if not os.path.exists(bin_path):
        return

    project_dir = env["PROJECT_DIR"]
    with open(bin_path, "rb") as fh:
        data = fh.read()
    sha = hashlib.sha256(data).hexdigest()

    branch = _git(project_dir, ["rev-parse", "--abbrev-ref", "HEAD"])
    if branch == "HEAD":
        branch = "detached"
    short = _git(project_dir, ["rev-parse", "--short", "HEAD"])
    base = _base_version(project_dir)

    version = f"{base}-dev-{branch}-{short}"
    manifest = {
        "target": "esp32c3",
        "product": "crosspoint-reader",
        "device": "X3/X4",  # one universal binary; runtime-detects the panel
        "branch": branch,
        "gitHash": short,
        "version": version,
        # The published binary's filename (version-stamped). The host installer fetches
        # this name from the same folder as the manifest; the build-dir output is plain
        # firmware.bin and publish-firmware.ps1 renames it to this on publish.
        "file": f"firmware-{version}.bin",
        "sha256": sha,
        "size": len(data),
    }

    out = os.path.join(build_dir, "firmware.manifest.json")
    with open(out, "w") as fh:
        json.dump(manifest, fh, indent=2)
    print(f"[gen_manifest] {out}  sha256={sha[:12]}...  size={len(data)}")


env.AddPostAction("$BUILD_DIR/firmware.bin", after_bin)  # noqa: F821
