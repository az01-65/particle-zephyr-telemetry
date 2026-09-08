#!/usr/bin/env python3
"""
mesh_visualizer.py -- live terminal view of the Thread mesh in action.

Reads the same JSON telemetry stream as tools/telemetry_monitor.py from the
Argon gateway's USB serial console, but instead of a scrolling log, renders a
redrawing dashboard: a topology diagram of the mesh actually in use (Argon as
the Thread Leader/gateway, each Xenon that's ever sent a sample as one of its
Children) plus a live per-node table.

What this honestly does and doesn't show:
  - The topology drawn is real, not decorative: this project's Xenons are
    built as MTD (Minimal Thread Devices), which are structurally unable to
    become anything other than a Child of the Argon - so "gateway with
    spokes to each node" is the actual mesh shape, not a simplification.
  - Per-node LIVE/STALE status is inferred purely from how recently a JSON
    line for that node_id arrived on this console - the same signal a human
    watching the raw log would use, just tracked automatically. It is not a
    read of Thread's own link-quality/routing state (see docs/ for how to
    inspect that over SWD if you need real RF diagnostics).
  - This works identically whether the gateway is currently relaying BLE or
    Thread telemetry - the JSON shape on the wire is the same either way, so
    this script can't tell you which radio mode produced a given line. Node
    IDs only exist in the Thread payload; a BLE-relayed line won't have one.

Usage:
    python3 tools/mesh_visualizer.py /dev/cu.usbmodem21402
    python3 tools/mesh_visualizer.py /dev/cu.usbmodem21402 --baud 115200

Requires pyserial (see tools/requirements.txt):
    pip install -r tools/requirements.txt
"""

import argparse
import json
import sys
import time

import serial

CLEAR_SCREEN = "\x1b[2J\x1b[H"
DIM = "\x1b[2m"
GREEN = "\x1b[32m"
YELLOW = "\x1b[33m"
RESET = "\x1b[0m"
BOLD = "\x1b[1m"

STALE_AFTER_S = 15.0
REDRAW_PERIOD_S = 0.5


def parse_args():
    parser = argparse.ArgumentParser(
        description="Live dashboard for the Argon gateway's telemetry stream."
    )
    parser.add_argument("port", help="Serial device path, e.g. /dev/cu.usbmodem21402")
    parser.add_argument("--baud", type=int, default=115200, help="Serial baud rate")
    parser.add_argument(
        "--stale-after",
        type=float,
        default=STALE_AFTER_S,
        help=f"Seconds without a sample before a node is shown as STALE (default: {STALE_AFTER_S})",
    )
    return parser.parse_args()


def fmt_age(seconds):
    if seconds < 1.0:
        return "just now"
    if seconds < 60.0:
        return f"{seconds:.0f}s ago"
    return f"{seconds / 60.0:.1f}m ago"


def render(nodes, total_count, start_time, stale_after, port):
    now = time.monotonic()
    lines = []

    lines.append(f"{BOLD}Particle Mesh Telemetry -- {port}{RESET}")
    lines.append(f"Uptime: {now - start_time:.0f}s   Total packets: {total_count}")
    lines.append("")

    # --- Topology: Argon is the fixed root. Every node_id ever seen becomes
    # one child, listed as an indented tree rather than drawn as a fixed-
    # width diagram - this mirrors the real, structurally-enforced star
    # shape of the mesh (see module docstring), and scales cleanly to any
    # number of nodes without alignment math.
    lines.append(f"{BOLD}Topology{RESET}")
    lines.append("")
    lines.append("Argon Gateway (Leader)")

    node_ids = sorted(nodes.keys())
    if not node_ids:
        lines.append(f"{DIM}   (no samples yet - waiting){RESET}")
    else:
        for i, node_id in enumerate(node_ids):
            info = nodes[node_id]
            age = now - info["last_seen"]
            live = age <= stale_after
            connector = "└─" if i == len(node_ids) - 1 else "├─"
            color = GREEN if live else DIM
            status = "LIVE " if live else "STALE"
            lines.append(f"{connector} {color}Xenon {node_id:<6} {status}  {fmt_age(age)}{RESET}")

    lines.append("")

    # --- Per-node table -------------------------------------------------
    lines.append(f"{BOLD}Nodes{RESET}")
    lines.append(f"{'NODE':<8} {'STATUS':<8} {'LAST SEEN':<12} {'TEMP (C)':>9} {'VDD (mV)':>9} {'SEQ':>6} {'COUNT':>7}")
    lines.append("-" * 66)

    if not node_ids:
        lines.append(f"{DIM}  (nothing received yet - is the gateway console open and a node transmitting?){RESET}")
    else:
        for node_id in node_ids:
            info = nodes[node_id]
            age = now - info["last_seen"]
            live = age <= stale_after
            status = f"{GREEN}LIVE{RESET}  " if live else f"{YELLOW}STALE{RESET} "
            temp_str = f"{info['temp_c']:.2f}" if info["temp_c"] is not None else "N/A"
            vdd_str = str(info["vdd_mv"]) if info["vdd_mv"] is not None else "N/A"
            seq_str = str(info["seq"]) if info["seq"] is not None else "N/A"
            lines.append(
                f"{node_id:<8} {status:<8} {fmt_age(age):<12} {temp_str:>9} {vdd_str:>9} "
                f"{seq_str:>6} {info['count']:>7}"
            )

    lines.append("")
    lines.append(f"{DIM}Ctrl+C to stop. Node status is inferred purely from packet timing --")
    lines.append(f"see this script's module docstring for exactly what is and isn't shown.{RESET}")

    sys.stdout.write(CLEAR_SCREEN + "\n".join(lines) + "\n")
    sys.stdout.flush()


def main():
    args = parse_args()

    try:
        ser = serial.Serial(args.port, args.baud, timeout=REDRAW_PERIOD_S)
    except serial.SerialException as exc:
        print(f"error: could not open serial port {args.port!r}: {exc}", file=sys.stderr)
        return 1

    nodes = {}
    total_count = 0
    start_time = time.monotonic()
    last_redraw = 0.0

    try:
        with ser:
            while True:
                raw = ser.readline()

                if raw:
                    line = raw.decode("utf-8", errors="replace").strip()
                    if line:
                        try:
                            reading = json.loads(line)
                        except json.JSONDecodeError:
                            reading = None

                        if isinstance(reading, dict):
                            node_id = reading.get("node_id", "BLE/?")
                            nodes.setdefault(
                                node_id,
                                {"temp_c": None, "vdd_mv": None, "seq": None, "count": 0, "last_seen": 0.0},
                            )
                            entry = nodes[node_id]
                            entry["temp_c"] = reading.get("temp_c", entry["temp_c"])
                            entry["vdd_mv"] = reading.get("vdd_mv", entry["vdd_mv"])
                            entry["seq"] = reading.get("seq", entry["seq"])
                            entry["count"] += 1
                            entry["last_seen"] = time.monotonic()
                            total_count += 1

                now = time.monotonic()
                if now - last_redraw >= REDRAW_PERIOD_S:
                    render(nodes, total_count, start_time, args.stale_after, args.port)
                    last_redraw = now
    except KeyboardInterrupt:
        print("\nStopped.")
        return 0

    return 0


if __name__ == "__main__":
    sys.exit(main())
