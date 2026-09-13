#!/usr/bin/env python3
"""Build the frozen Opus/FFT streaming benchmark without flashing it."""
import argparse
import os
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from support import build_experiment

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--signaling-url", default=os.environ.get("SIGNALING_URL"))
args = parser.parse_args()
if not args.signaling_url:
    parser.error("Set SIGNALING_URL or --signaling-url to your deployed Worker HTTPS origin")
raise SystemExit(build_experiment(
    Path(__file__).parent / "firmware", "fft-benchmark",
    ("esp-webrtc-solution", "esp-dsp", "esp-adf-libs"), args.signaling_url, rust=True))
