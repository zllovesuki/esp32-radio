#!/usr/bin/env python3
"""Build the archived C radio without flashing it."""
import argparse
import os
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from support import build_experiment

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--signaling-url", default=os.environ.get("SIGNALING_URL"))
parser.add_argument("--usb-signaling", action="store_true", help="Use the original host signaling bridge")
args = parser.parse_args()
if not args.usb_signaling and not args.signaling_url:
    parser.error("Set SIGNALING_URL or --signaling-url to your deployed Worker HTTPS origin")
raise SystemExit(build_experiment(
    Path(__file__).parent / "firmware", "legacy-c", ("esp-webrtc-solution",),
    None if args.usb_signaling else args.signaling_url))
