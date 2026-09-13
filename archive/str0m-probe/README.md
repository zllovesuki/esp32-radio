# str0m interoperability probe

A standalone Linux publisher and aiortc listener test Cloudflare SFU audio,
explicit negotiated channel IDs, reliable telemetry, loss-tolerant data, and
command replies. The publisher deliberately drops some outgoing DTLS application
datagrams after setup. Audio consists of synthetic 48 kHz stereo Opus silence.

This experiment creates independent SFU sessions using `REALTIME_APP_ID` and
`REALTIME_APP_TOKEN` from the root `.credential.env`. It closes its allocated
channels and tracks afterward. It does not access the board or deployed Worker.

## Run

From the repository root, with Rust 1.88 or later and Python 3.11:

```sh
python3.11 -m venv .tools/archive-python
.tools/archive-python/bin/pip install -r archive/str0m-probe/requirements.txt
cargo build --manifest-path archive/str0m-probe/Cargo.toml --locked --release
.tools/archive-python/bin/python archive/str0m-probe/sfu_probe.py
```

The script uses the archive's shared SFU HTTP helper. It generates a temporary
certificate for the Linux publisher; this is a test input, not a requirement to
provision device certificates from a computer. Private keys, session receipts,
and sanitized results go to ignored `artifacts/archive/str0m-probe/`.

## Layout and pins

`src/driver.rs` owns str0m and its UDP socket. `src/main.rs` exposes a bounded
JSON subprocess protocol to `sfu_probe.py`. `Cargo.lock` pins str0m 0.23.1 and
the RustCrypto provider 0.6.0. The copied provider's [patch notes](vendor/str0m-rust-crypto/PATCH.md)
explain its certificate-generation feature and explicit crypto selection.
This frozen copy is independent of the maintained firmware's provider.

The library can also be compiled with the project's Xtensa toolchain:

```sh
RUSTFLAGS='--cfg espidf_time64' cargo +esp-radio build \
  --manifest-path archive/str0m-probe/Cargo.toml --locked --lib --release \
  --target xtensa-esp32s3-espidf -Zbuild-std=std,panic_abort
```

That command generates target library code; it does not link or flash an
ESP-IDF application. Hardware support belongs to the maintained firmware.
