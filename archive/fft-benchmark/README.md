# FFT benchmark

Opus decoding and ESP-DSP transform benchmarks during SFU streaming.
The frozen Rust radio harness in `firmware/` supplies the streaming workload;
a C task on core 1 runs the benchmarks.

The benchmark checks known sine-wave bins, then measures 1,024- and 2,048-point
floating-point and fixed-point transforms over 2,500 audio packets per phase.
It reports decoder time, transform time, queue latency, dropped packets,
internal memory, and stack headroom. Bands from this benchmark go to the USB
console; the harness transmits precomputed spectrum.

The [maintained firmware](../../firmware/README.md) documents the Rust analysis
task, its ESP-DSP floating-point kernel, and lifetime contracts.

Follow the [archive setup](../README.md#setup), including `SIGNALING_URL`.
The frozen music reader requires a version 1 pack; archive preparation writes
it separately from the maintained radio's music:

```sh
.tools/archive-python/bin/python archive/prepare-music.py /path/to/audio.flac
python3 archive/fft-benchmark/build.py
.tools/archive-python/bin/python archive/flash.py fft-benchmark
```

Observe the native USB console with a serial monitor. `FFT_SELF_TEST`,
`FFT_READY`, `FFT_RESULT`, and `FFT_COMPLETE` identify the bounded measurement.
The two phases take approximately 100 seconds after packets start arriving.
Open the exhibit to add listeners when reproducing the streaming workload.
After `FFT_COMPLETE`, the benchmark retains allocations until reboot.

Record the audio input, listener count, and dependency pins when comparing
measurements. Keep serial logs under `artifacts/archive/fft-benchmark/` and
restore the maintained firmware and music using the archive guide.
