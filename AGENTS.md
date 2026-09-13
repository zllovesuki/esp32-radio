# Project guidance

Start with README.md for the project map and Makefile commands. Firmware-specific
ownership rules are in firmware/AGENTS.md and firmware/docs/ffi.md.
The active web application and its frontend/backend conventions are in
worker/README.md and worker/AGENTS.md.

Keep public documentation generic and focused on current behavior, architecture,
and developer workflows; do not embed personal usernames or deployment hosts.
Describe supported behavior directly; use before/after wording only to explain
compatibility constraints. Use ERRATA.md for integration issues and workarounds.
Keep detailed measurements, validation logs, research diaries and session history
in ignored artifacts/. Do not duplicate those records across guides.

Experiments must be explicit, reproducible and separate from the default firmware.
Selected historical experiments are versioned under archive/ with their own
guidance. Other local prototypes live in ignored experiments/; downloaded SDKs
and reference checkouts live in ignored .tools/. Maintained builds and CI must
not depend on either archive or experiment source. Keep supported tests and
synthetic fixtures in version control.
Preserve the working radio and its private credentials, music and original flash
backup. Describe hardware interruptions before flashing or resetting the board.
