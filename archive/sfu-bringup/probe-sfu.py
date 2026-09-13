#!/usr/bin/env python3
"""Follow echo-datachannels' SFU flow, with the S3 signaled over USB.

The SFU token stays on this computer. The S3 connects to the SFU over Wi-Fi.
The second WebRTC endpoint uses aiortc to validate the transport without a UI.
"""
import asyncio
import json
import os
from pathlib import Path
import sys
import re
import shlex
import time
import termios
from urllib.error import HTTPError
from urllib.request import Request, urlopen

from aiortc import RTCConfiguration, RTCPeerConnection, RTCSessionDescription
import serial

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
from serial_device import device_port
OUT = ROOT / "artifacts/archive/sfu-bringup"



class SFU:
    def __init__(self):
        os.umask(0o077)
        OUT.mkdir(parents=True, exist_ok=True, mode=0o700)
        values = {}
        for line in (ROOT / ".credential.env").read_text().splitlines():
            match = re.match(r"^\s*(?:export\s+)?([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(.*)$", line)
            if match:
                values[match.group(1)] = " ".join(shlex.split(match.group(2), comments=True))
        self.base = "https://rtc.live.cloudflare.com/v1/apps/" + values["REALTIME_APP_ID"]
        self.token = values["REALTIME_APP_TOKEN"]

    def request(self, path, body=None, method="POST"):
        request = Request(self.base + path,
            data=None if body is None else json.dumps(body).encode(), method=method,
            headers={"Authorization": "Bearer " + self.token,
                "Content-Type": "application/json", "User-Agent": "esp32-hardware-validation/0.1"})
        try:
            with urlopen(request, timeout=20) as response:
                result = json.load(response)
        except HTTPError as error:
            try:
                result = json.loads(error.read(2048))
            except (ValueError, UnicodeDecodeError):
                result = {}
            raise RuntimeError(f"SFU HTTP {error.code}: {result.get('errorCode', 'unknown_error')}") from None
        if result.get("errorCode"):
            raise RuntimeError(f"SFU: {result['errorCode']}")
        return result

    async def call(self, *args, **kwargs):
        return await asyncio.to_thread(self.request, *args, **kwargs)

    async def establish(self):
        session = await self.call("/sessions/new")
        sid = session["sessionId"]
        transport = await self.call(f"/sessions/{sid}/datachannels/establish", {
            "dataChannel": {"location": "remote", "dataChannelName": "server-events"}})
        if not transport.get("requiresImmediateRenegotiation"):
            raise RuntimeError("SFU transport did not request SDP negotiation")
        return sid, transport


class Device:
    def __init__(self):
        os.umask(0o077)
        OUT.mkdir(parents=True, exist_ok=True, mode=0o700)
        self.port = serial.Serial(port=None, baudrate=115200, timeout=0, write_timeout=5, exclusive=True)
        # Keep both lines asserted on open to avoid an intermediate USB-JTAG
        # reset state, and preserve them across close/reopen on Linux.
        self.port.dtr = True
        self.port.rts = True
        self.port.port = device_port()
        self.port.open()
        attrs = termios.tcgetattr(self.port.fileno())
        attrs[2] &= ~termios.HUPCL
        termios.tcsetattr(self.port.fileno(), termios.TCSANOW, attrs)
        self.pending = bytearray()
        self.events = []
        self.log = (OUT / "sfu-device.log").open("a")

    def send(self, **command):
        self.port.write(json.dumps(command).encode() + b"\n")
        self.port.flush()

    def poll(self):
        self.pending.extend(self.port.read(16384))
        while b"\n" in self.pending:
            raw, _, remainder = self.pending.partition(b"\n")
            self.pending = bytearray(remainder)
            line = raw.decode(errors="replace").strip()
            self.log.write(line + "\n")
            self.log.flush()
            if line.startswith("PROBE "):
                try:
                    event = json.loads(line[6:])
                except ValueError:
                    continue
                self.events.append(event)
                if event.get("event") in ("peer_state", "channel_open", "command_result"):
                    print("Device:", json.dumps(event), flush=True)

    async def wait(self, predicate, timeout=25):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self.poll()
            for index, event in enumerate(self.events):
                if predicate(event):
                    return self.events.pop(index)
            await asyncio.sleep(0.02)
        raise TimeoutError("Timed out waiting for device event")

    async def handshake(self):
        # Native USB can discard a command during the final port transition
        # after flashing. Require a reply before starting a new SFU session.
        for attempt in range(3):
            self.send(cmd="ping")
            try:
                return await self.wait(lambda e: e.get("cmd") == "ping" and e.get("result") == 0,
                    timeout=3 if attempt < 2 else 25)
            except TimeoutError:
                if attempt == 2:
                    raise

    def close(self):
        self.port.close()
        self.log.close()


async def main():
    api = SFU()
    device = Device()
    host = RTCPeerConnection(RTCConfiguration(iceServers=[]))
    registered = []
    result = {"s3_connected": False, "host_connected": False, "telemetry_received": False,
        "command_received": False, "led_driver_acknowledged": False}
    try:
        await device.handshake()
        device.send(cmd="peer_init", initiator=True)
        opened = await device.wait(lambda e: e.get("cmd") == "peer_init")
        if opened["result"]:
            raise RuntimeError(f"esp_peer_open failed: {opened['result']}")
        offer = await device.wait(lambda e: e.get("event") == "sdp")
        (OUT / "s3-offer.sdp").write_text(offer["text"])
        session = await api.call("/sessions/new", {
            "sessionDescription": {"type": "offer", "sdp": offer["text"]}})
        source = session["sessionId"]
        (OUT / "sfu-answer.sdp").write_text(session["sessionDescription"]["sdp"])
        device.send(cmd="sdp", text=session["sessionDescription"]["sdp"])
        await device.wait(lambda e: e.get("event") == "peer_state" and e.get("state") == 9)
        result["s3_connected"] = True
        print("S3 ICE, DTLS and SCTP connected to Cloudflare", flush=True)

        sink, transport = await api.establish()
        # The SFU opens server-events in-band using DCEP. Only application
        # channels returned by datachannels/new are externally negotiated.
        await host.setRemoteDescription(RTCSessionDescription(**transport["sessionDescription"]))
        await host.setLocalDescription(await host.createAnswer())
        await api.call(f"/sessions/{sink}/renegotiate", {
            "sessionDescription": {"type": "answer", "sdp": host.localDescription.sdp}}, method="PUT")
        for _ in range(1000):
            device.poll()
            if host.connectionState == "connected": break
            if host.connectionState in ("failed", "closed"): raise RuntimeError("Host WebRTC connection failed")
            await asyncio.sleep(0.02)
        if host.connectionState != "connected": raise TimeoutError("Host connection timeout")
        result["host_connected"] = True
        local = await api.call(f"/sessions/{source}/datachannels/new", {
            "dataChannels": [{"location": "local", "dataChannelName": "robot", "ordered": True}]})
        local_channel = local["dataChannels"][0]
        if local_channel.get("errorCode"): raise RuntimeError(local_channel["errorCode"])
        registered.append((source, local_channel["id"]))
        device.send(cmd="create_channel", label="robot", ordered=True)
        created = await device.wait(lambda e: e.get("cmd") == "create_channel")
        if created["result"] != 0:
            raise RuntimeError(f"Local DataChannel creation failed: {created['result']}")
        result["source_stream_id"] = local_channel["id"]
        remote = await api.call(f"/sessions/{sink}/datachannels/new", {
            "dataChannels": [{"location": "remote", "sessionId": source, "dataChannelName": "robot",
                "ordered": True, "waitForAck": True, "canReply": True}]})
        remote_channel = remote["dataChannels"][0]
        if remote_channel.get("errorCode"): raise RuntimeError(remote_channel["errorCode"])
        registered.append((sink, remote_channel["id"]))
        channel = host.createDataChannel("robot", negotiated=True, id=remote_channel["id"], ordered=True)
        incoming = asyncio.Queue()
        channel.on("message", incoming.put_nowait)
        for _ in range(500):
            if channel.readyState == "open": break
            await asyncio.sleep(0.02)
        if channel.readyState != "open": raise TimeoutError("Host DataChannel timeout")
        channel.send("ack")
        await asyncio.sleep(0.2)
        device.send(cmd="send", stream_id=local_channel["id"], text='{"sequence":1,"value":42}')
        sent = await device.wait(lambda e: e.get("cmd") == "send")
        result["esp_send_result"] = sent["result"]
        try:
            message = await asyncio.wait_for(incoming.get(), timeout=5)
            result["telemetry_received"] = json.loads(message).get("value") == 42
        except asyncio.TimeoutError:
            print("No telemetry arrived; testing reverse traffic independently", flush=True)
        channel.send('{"led":[16,0,16],"command_id":1}')
        try:
            reply = await device.wait(lambda e: e.get("event") == "data_received", timeout=8)
            result["command_received"] = json.loads(reply.get("text", "{}" )).get("command_id") == 1
            led = await device.wait(lambda e: e.get("event") == "led" and e.get("r") == 16, timeout=2)
            result["led_driver_acknowledged"] = led.get("result") == 0
        except TimeoutError:
            pass
    except Exception as error:
        result["error"] = f"{type(error).__name__}: {error}"
    finally:
        for sid, channel_id in reversed(registered):
            try:
                await api.call(f"/sessions/{sid}/datachannels/close", {"dataChannels": [{"id": channel_id}]}, method="PUT")
            except Exception:
                result["cleanup_incomplete"] = True
        await host.close()
        try:
            device.send(cmd="peer_close")
            closed = await device.wait(lambda e: e.get("cmd") == "peer_close", timeout=5)
            result["peer_close_acknowledged"] = closed.get("result") == 0
        except Exception:
            result["peer_close_acknowledged"] = False
        device.close()
        (OUT / "sfu-hardware-result.json").write_text(json.dumps(result, indent=2) + "\n")
        print(json.dumps(result, indent=2), flush=True)


if __name__ == "__main__":
    asyncio.run(main())
