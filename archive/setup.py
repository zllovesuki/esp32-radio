#!/usr/bin/env python3
"""Install the historical peer source needed by the archived hardware experiments."""
import subprocess

from support import ROOT, PINS
from project import verify_checkout


def main():
    destination = ROOT / ".tools/vendor/esp-webrtc-solution"
    revision = PINS["esp-webrtc-solution"]
    if not destination.exists():
        destination.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(["git", "init", str(destination)], check=True)
        subprocess.run(["git", "-C", str(destination), "remote", "add", "origin",
                        "https://github.com/espressif/esp-webrtc-solution.git"], check=True)
        subprocess.run(["git", "-C", str(destination), "fetch", "--depth", "1", "origin", revision], check=True)
        subprocess.run(["git", "-C", str(destination), "checkout", "--detach", "FETCH_HEAD"], check=True)
    verify_checkout(destination, revision)
    print("Archived peer dependency ready. No hardware was accessed.")


if __name__ == "__main__":
    main()
