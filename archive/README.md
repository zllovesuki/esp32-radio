# Archived experiments

Versioned reference implementations for the ESP32-S3-DevKitC-1 N32R16V,
excluded from the maintained build and CI.

| Project                                  | Purpose                                                                  |
| ---------------------------------------- | ------------------------------------------------------------------------ |
| [SFU bring-up](sfu-bringup/README.md)    | Hardware checks, data-channel creation, fan-out and reply permissions    |
| [FFT benchmark](fft-benchmark/README.md) | Opus decoding and floating-point/fixed-point FFT timing during streaming |
| [C radio](legacy-c/README.md)            | Single-song C radio with an optional USB signaling bridge                |
| [str0m probe](str0m-probe/README.md)      | Host SFU interoperability with explicit channel IDs and controlled packet loss |

## Setup

For the hardware experiments, complete the [root setup](../README.md#set-up) and preserve an
[original flash backup](../README.md#back-up-and-flash). Keep `SIGNALING_URL`
exported to the same Worker origin throughout the experiment and restoration.
The Python host tools use a separate environment:

```sh
python3 archive/setup.py
python3.11 -m venv .tools/archive-python
.tools/archive-python/bin/pip install -r archive/requirements.txt
```

`archive/setup.py` installs the pinned `esp_peer` checkout used by the older
hardware experiments. The maintained firmware uses str0m. The host-only str0m
probe has its own setup instructions and needs no ESP-IDF installation.

[dependencies.json](dependencies.json), the firmware lockfiles, and
[requirements.txt](requirements.txt) record the historical dependencies.
Builders verify these pins before using the installed SDK/vendor checkouts.
They reject incompatible revisions; they do not install a separate historical
SDK automatically.

Follow an individual project's build and run instructions. Building does not
flash. Explicit `archive/flash.py` commands interrupt the running board and
require a verified backup. Output goes under the configured SDK directory;
logs and generated media go to ignored `artifacts/archive/`.

## Native transport

The older hardware experiments use `esp_peer`. Its close operation can hang,
so recovery resets the board and peer configuration stays alive until reset.
Keep the validated 10 ms receive timeout; a 1 ms timeout caused DTLS disconnects.
The native SDK creates `robot` and `spectrum` in that order with stream IDs 2 and 4;
its creation API cannot select arbitrary negotiated IDs.

## Restore the radio

After an experiment, restore the maintained firmware and its prepared music catalog:

```sh
export SIGNALING_URL=https://radio.example.com
make build-firmware
make flash FLASH_ARGS=
```

Use your deployed origin. The restore reads the normal pack in `artifacts/radio/`.
Archive preparation writes a separate v1 pack, preserving the maintained playlist.
See [ERRATA](../ERRATA.md) for current integration and compatibility constraints.
