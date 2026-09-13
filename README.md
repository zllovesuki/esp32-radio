# Pocket Radio: ESP32 × Cloudflare SFU

An ESP32-S3 streams a shared music playlist through Cloudflare Realtime SFU.
Browsers listen together, watch the live spectrum and hardware telemetry, and
take turns controlling playback and the onboard RGB LED. The board needs only
USB power; sound plays in the browser.

The Rust firmware uses str0m to publish one stereo Opus track and two application
data channels.
`robot` carries telemetry and commands; `spectrum` carries 32 frequency bands.
A task on core 1 decodes a copy of the music and calls ESP-DSP's 2,048-point FFT.
A Worker and Durable Object coordinate sessions and control permission; media
flows through the SFU. The board's Wi-Fi network can be isolated from the
browser's LAN.

## Set up

The supported board is **ESP32-S3-DevKitC-1 N32R16V**: 32 MiB octal flash,
16 MiB octal PSRAM, and an RGB LED on GPIO38. Connect its native USB port.
The C6 is not a supported firmware target.

Use Linux x86_64, Node 24, Python 3.11 with venv support, and Rust 1.88 or later
with rustfmt and Clippy. Install the
[ESP-IDF Linux prerequisites](https://docs.espressif.com/projects/esp-idf/en/v5.5.3/esp32s3/get-started/linux-macos-setup.html#step-1-install-prerequisites).
Music preparation also needs `ffmpeg` and `ffprobe`.

Create a [Cloudflare Realtime SFU app](https://developers.cloudflare.com/realtime/sfu/get-started/)
and obtain its App ID and App Secret. From the repository root:

```sh
make setup
cp .credential.env.example .credential.env
# Fill in REALTIME_APP_ID, REALTIME_APP_TOKEN (the App Secret), and Wi-Fi credentials.
make secrets
export SIGNALING_URL=https://radio.example.com
```

Replace the example URL with your Worker's HTTPS origin. Keep it exported for
firmware builds, live previews, and archive restoration. Follow the
[Worker deployment guide](worker/README.md#deployment) to configure your domain
and deploy before starting the board.

Keep all credentials in ignored `.credential.env`. `make secrets` initializes
or imports the device token and viewer password. `worker/.dev.vars` is generated
automatically for web development and builds.

To change the viewer password, edit `VIEWER_PASSWORD` in `.credential.env`
and run `make deploy`. For local changes, restart `make dev`.

## Back up and flash

Your account needs serial-device access, commonly membership in `dialout` on
Debian/Ubuntu. Start a new login session after changing group membership.
Set `ESP32_PORT` if more than one board is connected.

Put your audio files in `tracks/` before preparing the first catalog. See
[playlist options](#change-the-playlist) for ordering and metadata overrides.
Before replacing the board's software:

```sh
make backup
make music TRACK=tracks/
make build-firmware
make flash FLASH_ARGS=
make monitor
```

`make backup` enters the bootloader, reads and verifies the full 32 MiB flash,
then records its SHA-256. It can take several minutes and refuses to overwrite
an existing backup. Files stay in `artifacts/hardware-validation/`; set
`RADIO_BACKUP_DIR` to use another directory, and keep separate backups for
separate boards. `make backup-check` checks saved files without contacting the
board. Flash helpers require a valid saved backup.

`make flash FLASH_ARGS=` writes the firmware and prepared music. Plain
`make flash` writes firmware only and preserves music. Verified images are
cached privately under
`artifacts/flash-cache/`, keyed by USB identity and partition layout. Later flashes
write changed sectors and verify the complete resulting image; a stale baseline
falls back to a full write. Use `FLASH_ARGS="--firmware-only --full"` for a complete
firmware write when troubleshooting.

`make monitor` streams filtered logs until interrupted.
`make monitor MONITOR_ARGS="--check --reset"` reboots and checks for a successful
heartbeat; a failed check exits with a nonzero status. Reconnect
browser listeners after the board restarts.

Open your Worker URL, enter the viewer password, and select **Start listening**.
**Take control** enables shared LED/playback controls. Volume and mute affect
only your browser.

## Change the playlist

Put audio files in ignored `tracks/`, then prepare them in filename order:

```sh
make music TRACK=tracks/
make flash-music
```

For a custom order or metadata overrides, copy [playlist.example.json](playlist.example.json)
to ignored `playlist.json`, edit its entries, and run `make music PLAYLIST=playlist.json`.
Preparation uses 48 kHz stereo Opus with a 96 kb/s variable-bitrate target.
The catalog supports up to 32 songs of at most 10 minutes each, with a total
duration of at most 60 minutes, and must fit the 16 MiB music partition.
See the [music format](firmware/docs/music.md) for packet
and metadata limits.

The board supplies the current title, artist, and duration; no Worker deployment
is needed when replacing the catalog.

The playlist loops, and the controller can use **Next track** to advance early.
Next preserves pause; Restart begins the current song and resumes playback.
The board reboots after a music flash. An installation with a 4 MiB music
partition needs a full firmware/partition-table flash before `make flash-music`; see
[compatibility](ERRATA.md#music-pack-compatibility).

## Development

`make fmt` formats Rust, C, and web source. The C formatter is installed by
`make setup`; `make setup-c-format` installs it independently of the ESP-IDF SDK.
Use `make fmt-c` or `make check-c-format` to format or check only the C adapters.

`make check` runs portable tests, type checks, formatting, Clippy, and rustdoc.
`make test-worker` checks the Worker lifecycle in the Cloudflare Vitest runtime.
`make test-worker-bundle` checks the built Worker and actual alarm scheduling.
These commands need no hardware or real credentials.

| Guide                                    | Contents                                               |
| ---------------------------------------- | ------------------------------------------------------ |
| [Worker](worker/README.md)               | Local preview, deployment, architecture, browser tests |
| [Firmware](firmware/README.md)           | Builds, task ownership, runtime and music format       |
| [Native boundary](firmware/docs/ffi.md)  | C/Rust buffer and lifetime contracts                   |
| [ERRATA](ERRATA.md)                      | Integration issues and compatibility                   |
| [Archives](archive/README.md)            | SFU bring-up, FFT benchmark, C radio, str0m probe      |
| [Third-party components](THIRD_PARTY.md) | Dependencies and licenses                              |

Maintained code lives in `firmware/`, `worker/`, and `scripts/`. Archives are
versioned but excluded from normal builds and CI. Downloads (`.tools/`), local
experiments (`experiments/`), and generated outputs (`artifacts/`) are ignored.
