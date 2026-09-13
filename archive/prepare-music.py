#!/usr/bin/env python3
"""Prepare private version 1 music for the archived C radio and FFT benchmark.
Requires ffmpeg, ffprobe and NumPy. Leaves the maintained radio's music pack alone.
"""
import argparse
import json
import os
from pathlib import Path
import struct
import subprocess
import unicodedata
import zlib


ROOT = Path(__file__).resolve().parents[1]
MAX_TAG_BYTES = 256


def music_tag(value, name, required=False):
    value = value.strip()
    if (required and not value) or len(value.encode("utf-8")) > MAX_TAG_BYTES:
        raise ValueError(f"{name} must be {'1' if required else '0'}..{MAX_TAG_BYTES} UTF-8 bytes; use --{name.lower()} to override it")
    if any(unicodedata.category(character) == "Cc" for character in value):
        raise ValueError(f"{name} contains control characters; use --{name.lower()} to override it")
    return value


def ogg_packets(data):
    cursor = 0
    packet = bytearray()
    while cursor < len(data):
        if data[cursor:cursor + 4] != b"OggS":
            raise ValueError("Invalid Ogg page")
        segments = data[cursor + 26]
        sizes = data[cursor + 27:cursor + 27 + segments]
        cursor += 27 + segments
        for size in sizes:
            packet.extend(data[cursor:cursor + size])
            cursor += size
            if size < 255:
                yield bytes(packet)
                packet.clear()
    if packet:
        raise ValueError("Truncated Ogg packet")


def legacy_spectrum(file, packets, preskip):
    import numpy as np
    pcm = np.frombuffer(subprocess.check_output([
        "ffmpeg", "-v", "error", "-i", str(file), "-map", "0:a:0", "-vn",
        "-ar", "48000", "-ac", "1", "-f", "f32le", "pipe:1"]), dtype="<f4")
    window_size = 2048
    window = np.hanning(window_size)
    padded = np.pad(pcm, (window_size, window_size))
    frequencies = np.fft.rfftfreq(window_size, 1 / 48000)
    edges = np.geomspace(30, 20000, 33)
    bins = [np.flatnonzero((frequencies >= low) & (frequencies < high))
            for low, high in zip(edges[:-1], edges[1:])]
    bins = [group if len(group) else np.array([np.argmin(abs(frequencies - (edges[i] + edges[i + 1]) / 2))])
            for i, group in enumerate(bins)]
    for index in range(len(packets)):
        center = index * 960 + 480 - preskip + window_size
        sample = padded[center - window_size // 2:center + window_size // 2]
        sample = np.pad(sample, (0, max(0, window_size - len(sample))))
        power = abs(np.fft.rfft(sample * window)) * 2 / window.sum()
        db = np.array([20 * np.log10(max(1e-6, power[group].max())) for group in bins])
        bands = np.clip((db + 72) / 66 * 255, 0, 255).astype(np.uint8).tobytes()
        yield bands


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("file", type=Path, nargs="?", default=os.environ.get("TRACK") or None)
    parser.set_defaults(legacy_spectrum=True)
    parser.add_argument("--title", help="Override the audio file's title tag")
    parser.add_argument("--artist", help="Override the audio file's artist tag")
    args = parser.parse_args()
    if args.file is None:
        parser.error("provide an audio file or set TRACK for make music")
    out = ROOT / "artifacts/archive/music"
    out.mkdir(parents=True, exist_ok=True, mode=0o700)
    metadata = json.loads(subprocess.check_output([
        "ffprobe", "-v", "error", "-show_format", "-of", "json", str(args.file)]))["format"]
    tags = {key.lower(): value for key, value in metadata.get("tags", {}).items()}
    title = music_tag(args.title if args.title is not None else tags.get("title", args.file.stem), "Title", required=True)
    artist = music_tag(args.artist if args.artist is not None else tags.get("artist", ""), "Artist")
    encoded = subprocess.check_output([
        "ffmpeg", "-v", "error", "-i", str(args.file), "-map", "0:a:0", "-vn",
        "-ar", "48000", "-ac", "2", "-c:a", "libopus", "-b:a", "96k",
        "-vbr", "on", "-frame_duration", "20", "-application", "audio", "-f", "opus", "pipe:1"])
    packets = list(ogg_packets(encoded))
    if not packets[0].startswith(b"OpusHead") or not packets[1].startswith(b"OpusTags"):
        raise ValueError("Expected Opus headers")
    preskip = struct.unpack_from("<H", packets[0], 10)[0]
    packets = packets[2:]
    legacy = legacy_spectrum(args.file, packets, preskip) if args.legacy_spectrum else None
    frames = bytearray()
    for index, packet in enumerate(packets):
        if not 0 < len(packet) <= 1275:
            raise ValueError("Unexpected Opus packet size")
        bands = next(legacy) if legacy is not None else b""
        frames.extend(struct.pack("<H", len(packet)) + bands + packet)
    if not 1 <= len(packets) <= 30000:
        raise ValueError("Track must contain 1..30000 audio frames")
    version = 1 if args.legacy_spectrum else 3
    tag_bytes = b"" if args.legacy_spectrum else (
        struct.pack("<HH", len(title.encode("utf-8")), len(artist.encode("utf-8")))
        + title.encode("utf-8") + artist.encode("utf-8"))
    payload = tag_bytes + frames
    header = struct.pack("<8sIIIIII32x", b"S3MUSIC\0", version, len(packets), 48000, 2, 20, zlib.crc32(payload))
    binary = header + payload
    if len(binary) > 4 * 1024 * 1024:
        raise ValueError("Music exceeds the configured 4 MiB partition")
    (out / "music.bin").write_bytes(binary)
    (out / "music.bin").chmod(0o600)
    manifest = {"title": title, "artist": artist,
                "durationMs": len(packets) * 20, "sourceDurationMs": round(float(metadata["duration"]) * 1000),
                "codec": "Opus", "sampleRate": 48000, "channels": 2, "frameMs": 20,
                "bands": 32, "frames": len(packets), "bytes": len(binary), "packVersion": version,
                "spectrum": "precomputed" if args.legacy_spectrum else "live-fft"}
    (out / "music.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(json.dumps(manifest, indent=2))


if __name__ == "__main__":
    main()
