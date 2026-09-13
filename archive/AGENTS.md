# Archive guidance

These are frozen, versioned experiments. Keep their dependency pins, distinct
purpose, and separate build output. Do not import archived source into the main
application or add archive hardware runs to normal CI.

Preserve the original workload when repairing build paths. In particular, the
FFT benchmark uses its older Rust radio harness, not the main application's
live FFT task. Keep explanatory READMEs focused on reproduction; output belongs in
ignored artifacts/archive/.

Build commands must not flash implicitly. Announce any explicit flashing or
hardware probe before running it. Preserve the original flash backup, current
firmware, and the maintained music pack. Secrets and SDP must stay out of logs
shown to the user and out of versioned source.
