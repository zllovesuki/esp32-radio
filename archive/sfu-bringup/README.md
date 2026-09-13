# SFU bring-up

Archived hardware diagnostics and host-driven WebRTC probes for the N32R16V.
The firmware checks PSRAM, Wi-Fi, time synchronization, verified HTTPS, and the
RGB LED. Python clients supply signaling over USB while the board's WebRTC
traffic travels over Wi-Fi to Cloudflare Realtime SFU.

The probes require explicit negotiated-channel creation in `robot`, then
`spectrum` order. The maintained firmware performs its own HTTPS signaling;
`worker/tests/live.mjs` checks the exhibit.

Follow the [archive setup](../README.md#setup), then build and explicitly flash:

```sh
python3 archive/sfu-bringup/build.py
.tools/archive-python/bin/python archive/flash.py sfu-bringup
.tools/archive-python/bin/python archive/sfu-bringup/monitor.py
```

Run one probe at a time against this diagnostic firmware:

```sh
.tools/archive-python/bin/python archive/sfu-bringup/investigate-datachannels.py
.tools/archive-python/bin/python archive/sfu-bringup/probe-datachannels.py
```

`investigate-datachannels.py` isolates explicit stream creation.
`probe-datachannels.py` exercises both reliability profiles, two subscribers,
LED control, and per-viewer reply permission. It does not retest revocation of
the first viewer; the maintained tests cover exclusive controller handoff.
`probe-sfu.py` contains the common host client and a single-subscriber probe.

These probes control the board and allocate real SFU sessions. Reboot between
probe runs: the native peer teardown may hang, as documented in
[archive transport notes](../README.md#native-transport). Results and any SDP captures
stay under ignored `artifacts/archive/sfu-bringup/`.
