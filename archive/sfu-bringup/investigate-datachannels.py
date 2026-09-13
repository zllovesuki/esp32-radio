#!/usr/bin/env python3
"""Bounded hardware experiment for explicit local DataChannel creation."""
import asyncio
import json
from pathlib import Path
import runpy
import time

from aiortc import RTCPeerConnection, RTCConfiguration, RTCSessionDescription

ROOT = Path(__file__).resolve().parents[2]
probe = runpy.run_path(str(Path(__file__).with_name("probe-sfu.py")))


async def main():
    api = probe["SFU"]()
    device = probe["Device"]()
    host = RTCPeerConnection(RTCConfiguration(iceServers=[]))
    resources = []
    result = {}
    try:
        device.send(cmd="ping")
        await device.wait(lambda e: e.get("cmd") == "ping")
        device.send(cmd="peer_init", initiator=True)
        initialized = await device.wait(lambda e: e.get("cmd") == "peer_init")
        if initialized["result"] != 0:
            raise RuntimeError(f"Device peer initialization: {initialized['result']}")
        offer = await device.wait(lambda e: e.get("event") == "sdp")
        source = await api.call("/sessions/new", {
            "sessionDescription": {"type": "offer", "sdp": offer["text"]}})
        source_id = source["sessionId"]
        device.send(cmd="sdp", text=source["sessionDescription"]["sdp"])
        await device.wait(lambda e: e.get("event") == "peer_state" and e.get("state") == 9)

        sink_id, transport = await api.establish()
        await host.setRemoteDescription(RTCSessionDescription(**transport["sessionDescription"]))
        await host.setLocalDescription(await host.createAnswer())
        await api.call(f"/sessions/{sink_id}/renegotiate", {
            "sessionDescription": {"type": "answer", "sdp": host.localDescription.sdp}}, method="PUT")
        deadline = time.monotonic() + 20
        while host.connectionState != "connected" and time.monotonic() < deadline:
            device.poll()
            await asyncio.sleep(0.02)
        if host.connectionState != "connected":
            raise RuntimeError("Host did not connect")

        created = await api.call(f"/sessions/{source_id}/datachannels/new", {
            "dataChannels": [{"location": "local", "dataChannelName": "robot", "ordered": True}]})
        local_id = created["dataChannels"][0]["id"]
        resources.append((source_id, local_id))
        pulled = await api.call(f"/sessions/{sink_id}/datachannels/new", {
            "dataChannels": [{"location": "remote", "sessionId": source_id, "dataChannelName": "robot",
                "ordered": True, "waitForAck": True, "canReply": True}]})
        remote_id = pulled["dataChannels"][0]["id"]
        resources.append((sink_id, remote_id))
        result.update(sfu_source_id=local_id, sfu_subscriber_id=remote_id)
        print("SFU IDs:", local_id, remote_id, flush=True)
        channel = host.createDataChannel("robot", negotiated=True, id=remote_id)
        received = []
        channel.on("message", received.append)
        deadline = time.monotonic() + 15
        while channel.readyState != "open":
            if time.monotonic() >= deadline or channel.readyState == "closed" or host.connectionState in {"failed", "closed"}:
                raise TimeoutError("Data channel did not open before the deadline")
            device.poll()
            await asyncio.sleep(0.02)
        channel.send("ack")

        device.send(cmd="create_channel")
        created = await device.wait(lambda e: e.get("cmd") == "create_channel")
        result["create_result"] = created["result"]
        deadline = time.monotonic() + 4
        while time.monotonic() < deadline:
            device.poll()
            await asyncio.sleep(0.02)
        result["opened_channels"] = [e for e in device.events if e.get("event") == "channel_open"]
        result["send_results"] = {}
        for stream_id in sorted({0, local_id}):
            device.send(cmd="send", stream_id=stream_id,
                text=json.dumps({"probe": "explicit_creation", "stream_id": stream_id}))
            sent = await device.wait(lambda e: e.get("cmd") == "send")
            result["send_results"][stream_id] = sent["result"]
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline:
            device.poll()
            await asyncio.sleep(0.02)
        result["received"] = [m.decode() if isinstance(m, bytes) else m for m in received]
    except Exception as exc:
        result["error"] = f"{type(exc).__name__}: {exc}"
    finally:
        for sid, cid in reversed(resources):
            try:
                await api.call(f"/sessions/{sid}/datachannels/close", {"dataChannels": [{"id": cid}]}, method="PUT")
            except Exception:
                result["cleanup_incomplete"] = True
        await host.close()
        device.close()
        out = ROOT / "artifacts/archive/sfu-bringup/explicit-channel-result.json"
        out.write_text(json.dumps(result, indent=2) + "\n")
        print(json.dumps(result, indent=2), flush=True)


if __name__ == "__main__":
    asyncio.run(main())
