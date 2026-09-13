"""Shared setup for archived experiments; never flashes or contacts the SFU."""
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
PINS = json.loads((Path(__file__).parent / "dependencies.json").read_text())
sys.path.insert(0, str(ROOT / "scripts"))
from project import idf_environment, sdk_root, verify_checkout
from config import read_env


def build_experiment(source, name, components, signaling_url=None, *, rust=False):
    sdk = sdk_root()
    verify_checkout(sdk / "esp-idf", PINS["esp-idf"])
    for component in components:
        verify_checkout(ROOT / ".tools/vendor" / component, PINS[component])
    if signaling_url:
        if not signaling_url.startswith("https://") or len(signaling_url) > 180:
            raise SystemExit("The signaling URL must use HTTPS and be at most 180 characters")
    if rust:
        version = subprocess.check_output(
            ["rustup", "run", "esp-radio", "rustc", "--version"], text=True)
        if f"({PINS['rust-release']})" not in version:
            raise SystemExit("Use the archived Xtensa Rust release from archive/dependencies.json")

    build = sdk / f"archive-{name}-build"
    private = build / "private"
    private.mkdir(parents=True, exist_ok=True)
    private.chmod(0o700)
    values = read_env(ROOT / ".credential.env")
    ssid = values.get("WIFI_SSID", "").encode()
    password = values.get("WIFI_PASSWORD", "").encode()
    if not 1 <= len(ssid) <= 32 or len(password) > 64:
        raise SystemExit("Check WIFI_SSID and WIFI_PASSWORD in .credential.env")

    def literal(value):
        return '"' + ''.join(f"\\x{byte:02x}" for byte in value) + '"'

    os.umask(0o077)
    header = private / "wifi_private.h"
    header.write_text("#pragma once\n" +
                      f"#define WIFI_SSID {literal(ssid)}\n" +
                      f"#define WIFI_PASSWORD {literal(password)}\n")
    header.chmod(0o600)
    token = ""
    if signaling_url:
        token = read_env(ROOT / "worker/.dev.vars").get("DEVICE_TOKEN", "")
        if not 32 <= len(token) <= 128:
            raise SystemExit("Run make secrets before building a streaming experiment")
    header = private / "signaling_private.h"
    header.write_text("#pragma once\n" +
                      f"#define SIGNALING_AUTONOMOUS {int(signaling_url is not None)}\n" +
                      f"#define SIGNALING_URL {literal((signaling_url or '').rstrip('/').encode())}\n" +
                      f"#define SIGNALING_DEVICE_TOKEN {literal(token.encode())}\n")
    header.chmod(0o600)
    python, env = idf_environment()
    return subprocess.run(
        [str(python), str(sdk / "esp-idf/tools/idf.py"), "-B", str(build), "build"],
        cwd=source, env=env).returncode
