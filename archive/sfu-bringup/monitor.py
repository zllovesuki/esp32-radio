#!/usr/bin/env python3
"""Read diagnostic output over native USB without toggling reset lines."""
import json
import os
from pathlib import Path
import sys
import re
import shlex
import time
import termios
import serial

root = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(root / "scripts"))
from serial_device import device_port
os.umask(0o077)
secrets = []
for line in (root / ".credential.env").read_text().splitlines():
    match = re.match(r"^\s*(?:export\s+)?([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(.*)$", line)
    if match:
        value = " ".join(shlex.split(match.group(2), comments=True))
        if value:
            secrets.append(value)
port = serial.Serial(port=None, baudrate=115200, timeout=0.2, exclusive=True)
port.dtr = True
port.rts = True
find_port = device_port
for attempt in range(40):
    try:
        port.port = find_port()
        port.open()
        attrs = termios.tcgetattr(port.fileno())
        attrs[2] &= ~termios.HUPCL
        termios.tcsetattr(port.fileno(), termios.TCSANOW, attrs)
        break
    except (serial.SerialException, RuntimeError):
        time.sleep(0.25)
else:
    raise SystemExit("USB serial port did not reappear")
pending = bytearray()
deadline = time.monotonic() + 80
out = root / "artifacts/archive/sfu-bringup"
out.mkdir(parents=True, exist_ok=True, mode=0o700)
ready = False
with (out / "hardware-serial.log").open("a") as log:
    while time.monotonic() < deadline:
        try:
            pending.extend(port.read(4096))
        except serial.SerialException:
            print("USB disconnected during the diagnostic run", flush=True)
            break
        while b"\n" in pending:
            raw, _, remainder = pending.partition(b"\n")
            pending = bytearray(remainder)
            line = raw.decode(errors="replace").strip()
            for value in secrets:
                line = line.replace(value, "[redacted]")
            log.write(line + "\n")
            log.flush()
            print(line, flush=True)
            if line.startswith("PROBE "):
                try:
                    event = json.loads(line[6:])
                except ValueError:
                    continue
                if event.get("event") == "hardware_ready":
                    (out / "hardware-result.json").write_text(json.dumps(event, indent=2) + "\n")
                    ready = True
        if ready:
            break
port.close()
if not ready:
    raise SystemExit("No diagnostic summary received within 80 seconds")
