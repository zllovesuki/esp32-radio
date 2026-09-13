# C radio

A C radio implementation for comparing language and ownership choices with the
maintained [Rust firmware](../../firmware/README.md). It streams Opus,
precomputed spectrum, and telemetry, and handles LED/playback commands.
The board does not publish track metadata.

## Autonomous signaling

Follow the [archive setup](../README.md#setup), including `SIGNALING_URL`, then
run from the repository root:

```sh
.tools/archive-python/bin/python archive/prepare-music.py /path/to/audio.flac
python3 archive/legacy-c/build.py
.tools/archive-python/bin/python archive/flash.py legacy-c
```

The maintained exhibit receives audio, precomputed spectrum, and command
acknowledgments. C telemetry lacks the required `firmware` field and is ignored
by the browser; the exhibit's explanations describe the maintained Rust firmware.

## USB signaling

The archived [bridge](run-device.py) supplies connection setup and heartbeats
over USB through the Worker API. WebRTC packets travel over Wi-Fi to the
SFU. To use this mode, rebuild and explicitly flash the C firmware:

```sh
python3 archive/legacy-c/build.py --usb-signaling
.tools/archive-python/bin/python archive/flash.py legacy-c
```

Run `make dev` in another terminal, then start the bridge:

```sh
.tools/archive-python/bin/python archive/legacy-c/run-device.py --url http://127.0.0.1:11880
```

Keep the bridge running while using `http://localhost:11880`. It uses the
versioned host client in `archive/sfu-bringup/` and credentials from
`worker/.dev.vars`. The bridge reboots the board by default. Its logs and SDP
captures stay in `artifacts/archive/legacy-c/`.

Use the [restore instructions](../README.md#restore-the-radio) afterward.
