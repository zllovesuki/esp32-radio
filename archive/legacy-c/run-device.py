#!/usr/bin/env python3
"""Supply USB signaling for the archived C radio through the Worker API."""
import argparse
import asyncio
import importlib.util
import json
from pathlib import Path
import sys
from urllib.error import HTTPError
from urllib.request import Request, urlopen

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
from config import read_env
spec = importlib.util.spec_from_file_location("probe", ROOT / "archive/sfu-bringup/probe-sfu.py")
probe = importlib.util.module_from_spec(spec)
spec.loader.exec_module(probe)
OUT = ROOT / "artifacts/archive/legacy-c"
probe.OUT = OUT
env = read_env(ROOT / "worker/.dev.vars")


class SignalingError(RuntimeError):
    def __init__(self, status, message):
        super().__init__(f"Signaling HTTP {status}: {message}")
        self.status = status


def request(base, path, data):
    req = Request(base + path, data=json.dumps(data).encode(), headers={
        "Content-Type": "application/json", "Authorization": "Bearer " + env["DEVICE_TOKEN"]})
    try:
        with urlopen(req, timeout=45) as response:
            return json.load(response)
    except HTTPError as error:
        try: message = json.loads(error.read(2048)).get("error", "request failed")
        except ValueError: message = "request failed"
        raise SignalingError(error.code, message) from None


async def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--url", default="http://127.0.0.1:11880")
    parser.add_argument("--no-reset", action="store_true", help="Use immediately after flashing a fresh board")
    args = parser.parse_args()
    device = probe.Device()
    try:
        await device.handshake()
        if not args.no_reset:
            device.send(cmd="restart_device")
            await asyncio.sleep(2)
            device.close()
            device = probe.Device()
            await device.handshake()
        device.send(cmd="peer_init")
        opened = await device.wait(lambda e: e.get("cmd") == "peer_init")
        if opened["result"] != 0:
            raise RuntimeError(f"S3 radio initialization failed ({opened['result']}). Check its music partition and restart the board.")
        offer = await device.wait(lambda e: e.get("event") == "sdp")
        started = await asyncio.to_thread(request, args.url, "/api/device/start", {
            "sessionDescription": {"type": "offer", "sdp": offer["text"]}})
        for name, sdp in (("s3-offer", offer["text"]), ("sfu-answer", started["sessionDescription"]["sdp"])):
            path = OUT / (name + ".sdp")
            path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
            path.touch(mode=0o600, exist_ok=True)
            path.chmod(0o600)
            path.write_text(sdp)
        generation = started["generation"]
        device.send(cmd="sdp", text=started["sessionDescription"]["sdp"])
        await device.wait(lambda e: e.get("event") == "peer_state" and e.get("state") == 9)
        created = await asyncio.to_thread(request, args.url, "/api/device/channels", {"generation": generation})
        ids = {}
        for name in ("robot", "spectrum"):
            channel = next(c for c in created["channels"] if c["dataChannelName"] == name)
            ids[name] = channel["id"]
            device.send(cmd="create_channel")
            reply = await device.wait(lambda e: e.get("cmd") == "create_channel")
            if reply["result"] != 0: raise RuntimeError(f"Local {name} creation failed")
        device.send(cmd="start", robot_id=ids["robot"], spectrum_id=ids["spectrum"])
        reply = await device.wait(lambda e: e.get("cmd") == "start")
        if reply["result"] != 0: raise RuntimeError("The device could not start playback")
        await asyncio.to_thread(request, args.url, "/api/device/ready", {"generation": generation})
        print("S3 radio is streaming Opus, telemetry and spectrum through Cloudflare. Open " + args.url, flush=True)
        print("USB carries setup and a service heartbeat only. Leave this helper running during local development.", flush=True)
        failures = 0
        while True:
            for _ in range(50):
                device.poll()
                device.events.clear()
                await asyncio.sleep(0.1)
            try:
                await asyncio.to_thread(request, args.url, "/api/device/heartbeat", {"generation": generation})
                if failures: print("Signaling heartbeat recovered.", flush=True)
                failures = 0
            except (SignalingError, OSError) as error:
                if isinstance(error, SignalingError) and error.status < 500:
                    raise
                failures += 1
                if failures >= 10: raise
                print(f"Signaling heartbeat temporarily unavailable; retry {failures}/10. The board keeps streaming.", flush=True)
    finally:
        # esp_peer_close currently hangs. A subsequent run restarts this demo
        # firmware, and the server closes the old SFU resources before setup.
        device.close()


if __name__ == "__main__":
    try: asyncio.run(main())
    except KeyboardInterrupt: print("Device helper stopped.")
