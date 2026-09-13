#!/usr/bin/env python3
"""Build archived N32R16V hardware/SFU diagnostics without flashing."""
import argparse
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from support import build_experiment

argparse.ArgumentParser(description=__doc__).parse_args()
raise SystemExit(build_experiment(
    Path(__file__).parent / "firmware", "sfu-bringup", ("esp-webrtc-solution",)))
