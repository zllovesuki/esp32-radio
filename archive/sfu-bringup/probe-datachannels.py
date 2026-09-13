#!/usr/bin/env python3
"""Exercise two S3 DataChannels, SFU fanout, and per-viewer reply permission."""
import asyncio
import json
from pathlib import Path
import runpy
import time

from aiortc import RTCPeerConnection, RTCConfiguration, RTCSessionDescription

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "artifacts/archive/sfu-bringup"
base = runpy.run_path(str(Path(__file__).with_name("probe-sfu.py")))
PROFILES = {
    "robot": {"ordered": True},
    "spectrum": {"ordered": False, "maxRetransmits": 0},
}


async def until(test, device, timeout=15):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        device.poll()
        if test():
            return
        await asyncio.sleep(0.01)
    raise TimeoutError("Condition not met before deadline")


async def main():
    api = base["SFU"]()
    device = base["Device"]()
    hosts = []
    resources = []
    local_channels = {}
    result = {"profiles": PROFILES, "messages_sent_per_profile": 100}
    try:
        await device.handshake()
        device.send(cmd="peer_init", initiator=True)
        response = await device.wait(lambda e: e.get("cmd") == "peer_init")
        if response["result"] != 0:
            raise RuntimeError("Peer initialization failed")
        offer = await device.wait(lambda e: e.get("event") == "sdp")
        source = await api.call("/sessions/new", {
            "sessionDescription": {"type": "offer", "sdp": offer["text"]}})
        source_id = source["sessionId"]
        device.send(cmd="sdp", text=source["sessionDescription"]["sdp"])
        await device.wait(lambda e: e.get("event") == "peer_state" and e.get("state") == 9)
        result["s3_connected"] = True

        for label, profile in PROFILES.items():
            created = await api.call(f"/sessions/{source_id}/datachannels/new", {
                "dataChannels": [{"location": "local", "dataChannelName": label, **profile}]})
            item = created["dataChannels"][0]
            if item.get("errorCode"):
                raise RuntimeError(item["errorCode"])
            local_channels[label] = item["id"]
            resources.append((source_id, item["id"]))
            # Register local send state too; enabling the SCTP transport alone
            # does not create a channel when manual_ch_create is enabled.
            command = {"cmd": "create_channel", "label": label, "ordered": profile["ordered"]}
            if "maxRetransmits" in profile:
                command["max_retransmits"] = profile["maxRetransmits"]
            device.send(**command)
            created_local = await device.wait(lambda e: e.get("cmd") == "create_channel")
            if created_local["result"] != 0:
                raise RuntimeError(f"Local {label} creation: {created_local['result']}")
        result["source_stream_ids"] = local_channels

        for viewer in range(2):
            peer = RTCPeerConnection(RTCConfiguration(iceServers=[]))
            endpoint = {"peer": peer, "channels": {}, "received": {label: [] for label in PROFILES}}
            hosts.append(endpoint)
            sid, transport = await api.establish()
            endpoint["session_id"] = sid
            await peer.setRemoteDescription(RTCSessionDescription(**transport["sessionDescription"]))
            await peer.setLocalDescription(await peer.createAnswer())
            await api.call(f"/sessions/{sid}/renegotiate", {
                "sessionDescription": {"type": "answer", "sdp": peer.localDescription.sdp}}, method="PUT")
            await until(lambda: peer.connectionState == "connected", device)
            for label, profile in PROFILES.items():
                pulled = await api.call(f"/sessions/{sid}/datachannels/new", {
                    "dataChannels": [{"location": "remote", "sessionId": source_id, "dataChannelName": label,
                        "waitForAck": True, "canReply": viewer == 0 and label == "robot", **profile}]})
                item = pulled["dataChannels"][0]
                if item.get("errorCode"):
                    raise RuntimeError(item["errorCode"])
                resources.append((sid, item["id"]))
                channel = peer.createDataChannel(label, negotiated=True, id=item["id"], **profile)
                endpoint["channels"][label] = channel
                channel.on("message", endpoint["received"][label].append)
                await until(lambda: channel.readyState == "open", device)
                channel.send("subscriber-ready")
        print("Both viewers connected; sending two profiles", flush=True)
        start = time.monotonic()
        for sequence in range(100):
            for label in PROFILES:
                payload = {"kind": label, "sequence": sequence}
                if label == "robot":
                    payload["value"] = sequence * 17 % 101
                else:
                    payload["bands"] = [(sequence * 7 + band * 11) % 256 for band in range(32)]
                device.send(cmd="send", stream_id=local_channels[label], text=json.dumps(payload))
                sent = await device.wait(lambda e: e.get("cmd") == "send")
                if sent["result"] != 0:
                    raise RuntimeError(f"Send on {label}: {sent['result']}")
            await asyncio.sleep(max(0, start + (sequence + 1) * 0.05 - time.monotonic()))
        result["send_duration_seconds"] = round(time.monotonic() - start, 3)
        await until(lambda: all(len(host["received"]["robot"]) == 100 for host in hosts), device)
        await asyncio.sleep(0.5)
        result["receivers"] = []
        for host in hosts:
            metrics = {}
            for label, messages in host["received"].items():
                decoded = [json.loads(msg) for msg in messages]
                sequence = [msg["sequence"] for msg in decoded]
                metrics[label] = {"received": len(messages), "unique": len(set(sequence)),
                    "all_in_order": sequence == list(range(100)),
                    "payloads_valid": all(msg.get("kind") == label and
                        (msg.get("value") == msg["sequence"] * 17 % 101 if label == "robot" else
                         msg.get("bands") == [(msg["sequence"] * 7 + band * 11) % 256 for band in range(32)])
                        for msg in decoded)}
            result["receivers"].append(metrics)

        async def command(viewer, command_id, color):
            started = time.monotonic()
            hosts[viewer]["channels"]["robot"].send(json.dumps({"led": color, "command_id": command_id}))
            received = await device.wait(lambda e: e.get("event") == "data_received" and
                json.loads(e.get("text", "{}" )).get("command_id") == command_id, timeout=5)
            led = await device.wait(lambda e: e.get("event") == "led" and
                [e.get("r"), e.get("g"), e.get("b")] == color, timeout=2)
            return {"received": bool(received), "led_result": led["result"],
                "observed_ms_including_usb": round((time.monotonic() - started) * 1000, 1)}

        result["controller_command"] = await command(0, 101, [16, 0, 16])
        hosts[1]["channels"]["robot"].send('{"led":[16,0,0],"command_id":999}')
        deadline = time.monotonic() + 1
        while time.monotonic() < deadline:
            device.poll()
            await asyncio.sleep(0.02)
        result["spectator_command_blocked"] = not any(e.get("event") == "data_received" and
            json.loads(e.get("text", "{}" )).get("command_id") == 999 for e in device.events)
        await api.call(f"/sessions/{hosts[1]['session_id']}/datachannels/update", {
            "dataChannels": [{"location": "remote", "sessionId": source_id,
                "dataChannelName": "robot", "canReply": True}]}, method="PUT")
        result["new_controller_command"] = await command(1, 102, [0, 16, 0])
    except Exception as exc:
        result["error"] = f"{type(exc).__name__}: {exc}"
    finally:
        result["local_channel_close_results"] = {}
        for label in reversed(local_channels):
            try:
                device.send(cmd="close_channel", label=label)
                closed = await device.wait(lambda e: e.get("cmd") == "close_channel", timeout=3)
                result["local_channel_close_results"][label] = closed["result"]
            except Exception:
                result["local_channel_close_results"][label] = "timeout"
        # Keep polling the protocol loop while stream-reset acknowledgments arrive.
        end = time.monotonic() + 1
        while time.monotonic() < end:
            device.poll()
            await asyncio.sleep(0.02)
        result["channel_closed_events"] = [e for e in device.events if e.get("event") == "channel_closed"]
        for sid, cid in reversed(resources):
            try:
                await api.call(f"/sessions/{sid}/datachannels/close", {"dataChannels": [{"id": cid}]}, method="PUT")
            except Exception:
                result["cleanup_incomplete"] = True
        for host in hosts:
            await host["peer"].close()
        try:
            device.send(cmd="peer_close")
            closed = await device.wait(lambda e: e.get("cmd") == "peer_close", timeout=5)
            result["peer_close_result"] = closed["result"]
        except Exception:
            result["peer_close_result"] = "timeout"
        device.close()
        (OUT / "datachannels-result.json").write_text(json.dumps(result, indent=2) + "\n")
        print(json.dumps(result, indent=2), flush=True)


if __name__ == "__main__":
    asyncio.run(main())
