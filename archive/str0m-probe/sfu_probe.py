"""Isolated str0m publisher / aiortc subscriber with synthetic audio and data."""

import asyncio
from datetime import datetime, timedelta, timezone
import importlib.util
import json
import os
from pathlib import Path
import socket

from aiortc import RTCConfiguration, RTCPeerConnection, RTCSessionDescription
from aiortc.mediastreams import MediaStreamError
from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.x509.oid import NameOID

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "artifacts/archive/str0m-probe"
OUT.mkdir(parents=True, exist_ok=True)
os.umask(0o077)
spec = importlib.util.spec_from_file_location("sfu_api", ROOT / "archive/sfu-bringup/probe-sfu.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


async def until(predicate, timeout=20):
    deadline = asyncio.get_running_loop().time() + timeout
    while not predicate():
        if asyncio.get_running_loop().time() >= deadline:
            raise TimeoutError("bounded condition")
        await asyncio.sleep(0.02)


def certificate():
    key = ec.generate_private_key(ec.SECP256R1())
    name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "str0m feasibility probe")])
    now = datetime.now(timezone.utc)
    cert = (x509.CertificateBuilder().subject_name(name).issuer_name(name)
            .public_key(key.public_key()).serial_number(x509.random_serial_number())
            .not_valid_before(now - timedelta(minutes=1)).not_valid_after(now + timedelta(days=1))
            .sign(key, hashes.SHA256()))
    cert_path, key_path = OUT / "probe-cert.der", OUT / "probe-key.der"
    cert_path.write_bytes(cert.public_bytes(serialization.Encoding.DER))
    key_path.write_bytes(key.private_bytes(serialization.Encoding.DER, serialization.PrivateFormat.PKCS8, serialization.NoEncryption()))
    return cert_path, key_path


async def main():
    api = module.SFU()
    subscriber = RTCPeerConnection(RTCConfiguration(iceServers=[]))
    allocated = []
    tracks = []
    processes = []
    consumers = []
    peer_events = []
    telemetry, spectrum, replies = [], [], []
    audio = {"frames": 0, "samples": 0, "rates": set(), "channels": set()}
    result = {"board_access": False, "cleanup": []}
    stage = "initialize"

    def journal():
        (OUT / "sfu-private.json").write_text(json.dumps({"channels": allocated, "tracks": tracks}, indent=2) + "\n")

    async def send(value):
        process.stdin.write((json.dumps(value) + "\n").encode())
        await process.stdin.drain()

    async def event(wanted, timeout=20):
        deadline = asyncio.get_running_loop().time() + timeout
        while True:
            line = await asyncio.wait_for(process.stdout.readline(), max(0.1, deadline - asyncio.get_running_loop().time()))
            if not line:
                raise RuntimeError("publisher exited")
            value = json.loads(line)
            if value.get("event") == "failed":
                raise RuntimeError("publisher failed")
            if value.get("event") != "offer":
                peer_events.append(value)
            if value.get("event") == wanted:
                return value

    async def create_channels(sid, configs):
        response = await api.call(f"/sessions/{sid}/datachannels/new", {"dataChannels": configs})
        for channel in response.get("dataChannels", []):
            if isinstance(channel.get("id"), int):
                allocated.append({"sessionId": sid, "id": channel["id"]})
        journal()
        if len(response.get("dataChannels", [])) != len(configs) or any(x.get("errorCode") for x in response["dataChannels"]):
            raise RuntimeError("channel registration failed")
        return {item["dataChannelName"]: item["id"] for item in response["dataChannels"]}

    async def consume(track):
        try:
            while True:
                frame = await track.recv()
                audio["frames"] += 1
                audio["samples"] += frame.samples
                audio["rates"].add(frame.sample_rate)
                audio["channels"].add(len(frame.layout.channels))
        except (MediaStreamError, asyncio.CancelledError):
            pass

    @subscriber.on("track")
    def on_track(track):
        if track.kind == "audio":
            consumers.append(asyncio.create_task(consume(track)))

    try:
        cert, key = certificate()
        probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        probe.connect(("1.1.1.1", 80))
        local_ip = probe.getsockname()[0]
        probe.close()
        stage = "start publisher"
        process = await asyncio.create_subprocess_exec(
            str(Path(__file__).parent / "target/release/str0m-probe"), local_ip, str(cert), str(key),
            stdin=asyncio.subprocess.PIPE, stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.DEVNULL)
        processes.append(process)
        offer = await event("offer")
        result["offer_has_stereo_opus"] = "opus/48000/2" in offer["sdp"].lower()
        source = (await api.call("/sessions/new"))["sessionId"]
        stage = "register publisher audio"
        published = await api.call(f"/sessions/{source}/tracks/new", {
            "sessionDescription": {"type": "offer", "sdp": offer["sdp"]},
            "tracks": [{"location": "local", "mid": offer["mid"], "trackName": "str0m-probe-audio"}],
        })
        tracks.extend({"sessionId": source, "mid": item["mid"]} for item in published.get("tracks", []) if item.get("mid"))
        result["published_audio"] = [{k: item[k] for k in ("mid", "trackName", "errorCode") if k in item} for item in published.get("tracks", [])]
        journal()
        if any(item.get("errorCode") for item in published.get("tracks", [])):
            raise RuntimeError("publisher audio rejected")
        await send({"command": "answer", "sdp": published["sessionDescription"]["sdp"]})
        stage = "publisher connection"
        await event("connected")
        stage = "publisher channels"
        local = await create_channels(source, [
            {"location": "local", "dataChannelName": "padding-a"},
            {"location": "local", "dataChannelName": "robot", "ordered": True},
            {"location": "local", "dataChannelName": "padding-b"},
            {"location": "local", "dataChannelName": "spectrum", "ordered": False, "maxRetransmits": 0},
        ])
        result["publisher_ids"] = {name: local[name] for name in ("robot", "spectrum")}
        await send({"command": "channels", "robot": local["robot"], "spectrum": local["spectrum"]})

        stage = "subscriber connection"
        sink, transport = await api.establish()
        await subscriber.setRemoteDescription(RTCSessionDescription(**transport["sessionDescription"]))
        await subscriber.setLocalDescription(await subscriber.createAnswer())
        await api.call(f"/sessions/{sink}/renegotiate", {"sessionDescription": {"type": "answer", "sdp": subscriber.localDescription.sdp}}, method="PUT")
        await until(lambda: subscriber.connectionState == "connected")
        remote = await create_channels(sink, [
            {"location": "remote", "sessionId": source, "dataChannelName": "robot", "ordered": True, "waitForAck": True, "canReply": True},
            {"location": "remote", "sessionId": source, "dataChannelName": "spectrum", "ordered": False, "maxRetransmits": 0, "waitForAck": True},
        ])
        result["subscriber_ids"] = remote
        robot = subscriber.createDataChannel("robot", negotiated=True, id=remote["robot"], ordered=True)
        band = subscriber.createDataChannel("spectrum", negotiated=True, id=remote["spectrum"], ordered=False, maxRetransmits=0)

        @robot.on("message")
        def robot_message(message):
            item = json.loads(message)
            (telemetry if item.get("kind") == "telemetry" else replies).append(item)

        @band.on("message")
        def spectrum_message(message):
            if isinstance(message, bytes) and len(message) == 4:
                spectrum.append(int.from_bytes(message, "little"))

        await until(lambda: robot.readyState == "open" and band.readyState == "open")
        robot.send("ready")
        band.send("ready")
        stage = "audio warmup"
        await send({"command": "warmup", "frames": 25})
        result["warmup"] = (await event("warmup_done"))["stats"]
        stage = "subscriber audio"
        pulled = await api.call(f"/sessions/{sink}/tracks/new", {"tracks": [{"location": "remote", "sessionId": source, "trackName": "str0m-probe-audio"}]})
        tracks.extend({"sessionId": sink, "mid": item["mid"]} for item in pulled.get("tracks", []) if item.get("mid"))
        result["pulled_audio"] = [{k: item[k] for k in ("mid", "trackName", "errorCode") if k in item} for item in pulled.get("tracks", [])]
        journal()
        if any(item.get("errorCode") for item in pulled.get("tracks", [])):
            raise RuntimeError("subscriber audio rejected")
        if pulled.get("sessionDescription"):
            await subscriber.setRemoteDescription(RTCSessionDescription(**pulled["sessionDescription"]))
            await subscriber.setLocalDescription(await subscriber.createAnswer())
            await api.call(f"/sessions/{sink}/renegotiate", {"sessionDescription": {"type": "answer", "sdp": subscriber.localDescription.sdp}}, method="PUT")
        else:
            raise RuntimeError("subscriber audio offer missing")
        stage = "publisher channel readiness"
        while not {"robot", "spectrum"}.issubset({e.get("label") for e in peer_events if e.get("event") == "channel_open"}):
            await event("channel_open")
        await asyncio.sleep(0.3)
        stage = "synthetic workload"
        frames = 300
        await send({"command": "run", "frames": frames, "drop_every": 5})
        for sequence in range(5):
            robot.send(json.dumps({"kind": "command", "sequence": sequence}))
            await asyncio.sleep(0.1)
        done = await event("run_done")
        await until(lambda: len(telemetry) == frames // 5 and len(replies) == 5, timeout=15)
        await send({"command": "stats"})
        stats = (await event("stats"))["stats"]
        result.update(
            sender_stats=stats,
            audio={k: sorted(v) if isinstance(v, set) else v for k, v in audio.items()},
            reliable_received=len(telemetry),
            reliable_in_order=[x["sequence"] for x in telemetry] == list(range(frames // 5)),
            unreliable_received=len(spectrum),
            unreliable_unique=len(set(spectrum)),
            command_round_trips=len(replies),
            peer_events=peer_events,
        )
        result["passed"] = (
            result["offer_has_stereo_opus"] and result["reliable_in_order"] and len(replies) == 5
            and 0 < len(spectrum) < frames // 2 and len(spectrum) == len(set(spectrum))
            and audio["frames"] > 200 and audio["rates"] == {48000} and audio["channels"] == {2}
            and stats["deliberately_dropped"] > 0
        )
    except Exception as error:
        result["passed"] = False
        result["error"] = {"stage": stage, "type": type(error).__name__}
    finally:
        for item in reversed(allocated):
            try:
                await api.call(f"/sessions/{item['sessionId']}/datachannels/close", {"dataChannels": [{"id": item["id"]}]}, method="PUT")
                result["cleanup"].append({"kind": "channel", "closed": True})
            except Exception:
                result["cleanup"].append({"kind": "channel", "closed": False})
        for item in reversed(tracks):
            try:
                await api.call(f"/sessions/{item['sessionId']}/tracks/close", {"tracks": [{"mid": item["mid"]}], "force": True}, method="PUT")
                result["cleanup"].append({"kind": "track", "closed": True})
            except Exception:
                result["cleanup"].append({"kind": "track", "closed": False})
        for process in processes:
            if process.returncode is None:
                try:
                    await send({"command": "stop"})
                    await asyncio.wait_for(process.wait(), timeout=5)
                except Exception:
                    process.kill()
                    await process.wait()
        await subscriber.close()
        for consumer in consumers:
            consumer.cancel()
        await asyncio.gather(*consumers, return_exceptions=True)
        result["cleanup_complete"] = all(x["closed"] for x in result["cleanup"])
        (OUT / "sfu-result.json").write_text(json.dumps(result, indent=2) + "\n")
        print(json.dumps({k: v for k, v in result.items() if k != "peer_events"}, indent=2))
    return 0 if result.get("passed") and result["cleanup_complete"] else 1


if __name__ == "__main__":
    raise SystemExit(asyncio.run(main()))
