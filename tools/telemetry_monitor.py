#!/usr/bin/env python3
"""
telemetry_monitor.py -- live monitor for BLE sensor telemetry over serial.

The Argon gateway firmware decodes BLE advertisements/notifications from a
Xenon sensor node and prints one JSON object per line to its USB serial
console, shaped like:

    {"node_id": "0x4a", "temp_c": 23.50, "vdd_mv": 3300, "seq": 12}

node_id identifies which physical Xenon a sample came from - every Xenon
runs the identical firmware image, so this is derived from each board's own
factory-programmed hardware ID rather than being baked in per-build.

The same serial stream also carries Zephyr LOG_INF debug lines and other
boot-time chatter, which are NOT valid JSON. This script reads the serial
port line by line, tries to parse each line as JSON, and pretty-prints only
the lines that are valid telemetry readings, ignoring everything else.

Usage:
    python3 tools/telemetry_monitor.py /dev/cu.usbmodem21402
    python3 tools/telemetry_monitor.py /dev/cu.usbmodem21402 --baud 115200

Requires pyserial (see tools/requirements.txt):
    pip install -r tools/requirements.txt
"""

import argparse
import json
import sys
from datetime import datetime

import serial


def parse_args():
    parser = argparse.ArgumentParser(
        description="Monitor JSON telemetry lines from an Argon gateway's serial console."
    )
    parser.add_argument(
        "port",
        help="Serial device path, e.g. /dev/cu.usbmodem21402",
    )
    parser.add_argument(
        "--baud",
        type=int,
        default=115200,
        help="Serial baud rate (default: 115200)",
    )
    return parser.parse_args()


def print_header():
    print(f"{'TIME':<12} {'NODE':>6} {'TEMP (C)':>10} {'VDD (mV)':>10} {'SEQ':>8}")
    print("-" * 51)


def print_reading(reading):
    """Right-align/pad a telemetry reading into a single readable line.

    Unexpected/missing fields are tolerated by falling back to 'N/A' rather
    than crashing, since the exact schema is defined by firmware we don't
    control here. node_id is absent on samples from older firmware without
    per-board IDs, so it falls back the same way.
    """
    now = datetime.now().strftime("%H:%M:%S")

    node_id = reading.get("node_id")
    temp_c = reading.get("temp_c")
    vdd_mv = reading.get("vdd_mv")
    seq = reading.get("seq")

    node_str = str(node_id) if node_id is not None else "N/A"
    temp_str = f"{temp_c:.2f}" if isinstance(temp_c, (int, float)) else "N/A"
    vdd_str = str(vdd_mv) if vdd_mv is not None else "N/A"
    seq_str = str(seq) if seq is not None else "N/A"

    print(f"{now:<12} {node_str:>6} {temp_str:>10} {vdd_str:>10} {seq_str:>8}")


def main():
    args = parse_args()

    try:
        ser = serial.Serial(args.port, args.baud, timeout=1)
    except serial.SerialException as exc:
        print(f"error: could not open serial port {args.port!r}: {exc}", file=sys.stderr)
        return 1

    print(f"Listening on {args.port} @ {args.baud} baud (Ctrl+C to stop)")
    print_header()

    try:
        with ser:
            while True:
                raw = ser.readline()
                if not raw:
                    # Timeout with no data; keep waiting.
                    continue

                line = raw.decode("utf-8", errors="replace").strip()
                if not line:
                    continue

                try:
                    reading = json.loads(line)
                except json.JSONDecodeError:
                    # Not JSON -- almost certainly a LOG_INF/boot line. Ignore it.
                    continue

                if not isinstance(reading, dict):
                    # Valid JSON but not a telemetry object (e.g. a bare number).
                    continue

                print_reading(reading)
    except KeyboardInterrupt:
        print("\nStopped.")
        return 0

    return 0


if __name__ == "__main__":
    sys.exit(main())
