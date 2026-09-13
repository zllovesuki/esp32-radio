#!/usr/bin/env python3
"""Explicitly flash one archived experiment. This interrupts the running radio."""
import argparse
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
from project import sdk_root
from serial_device import device_port
from flash_backup import require_backup

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("experiment", choices=["sfu-bringup", "fft-benchmark", "legacy-c"])
args = parser.parse_args()
try:
    require_backup()
except ValueError as error:
    raise SystemExit(str(error)) from None
build = sdk_root() / f"archive-{args.experiment}-build"
name = "s3_hardware_check.bin" if args.experiment == "sfu-bringup" else "s3_radio.bin"
files = [("0x0", build / "bootloader/bootloader.bin"),
         ("0x8000", build / "partition_table/partition-table.bin"),
         ("0x10000", build / name)]
if args.experiment in {"fft-benchmark", "legacy-c"}:
    music = ROOT / "artifacts/archive/music/music.bin"
    header = music.read_bytes()[:64]
    if len(header) != 64 or header[:8] != b"S3MUSIC\0" or int.from_bytes(header[8:12], "little") != 1 or music.stat().st_size > 4 * 1024 * 1024:
        raise SystemExit("Prepare a version 1 pack with archive/prepare-music.py")
    files.append(("0x410000", music))
if any(not path.is_file() for _, path in files):
    raise SystemExit("Build the selected archived experiment first")
port = device_port()
command = [sys.executable, "-m", "esptool", "--chip", "esp32s3", "--port", port,
           "--baud", "460800", "--before", "default-reset", "--after", "hard-reset",
           "write-flash", "--flash-mode", "dout", "--flash-size", "32MB", "--flash-freq", "80m"]
for address, path in files:
    command.extend([address, str(path)])
raise SystemExit(subprocess.run(command).returncode)
