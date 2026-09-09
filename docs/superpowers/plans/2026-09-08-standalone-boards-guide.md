# Standalone Boards Guide Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ship a complete, honest, professionally-organized guide to repurposing deprecated Particle Argon/Xenon boards, adding one new genuinely-robust no-Arduino-IDE flashing path (UF2 drag-and-drop), regression-testing everything that can be tested without physical intervention, rewriting the README around the 5 concrete things this repo demonstrates, and shipping it as a squash-merged release to `master`.

**Architecture:** Two new pieces (a from-scratch UF2 converter + a minimal cross-board Zephyr template app) sit alongside the three firmware images that already exist (`argon_gateway`, `xenon_sensor`, `esp32_passthrough`). All five become the "5 examples" the README is organized around. UF2 drag-and-drop becomes the recommended non-Arduino upload path because it needs no debug probe and no OpenOCD once a board's bootloader is in place — genuinely more robust than both Arduino IDE's Upload button and, for iteration speed, comparable to SWD without the toolchain requirement.

**Tech Stack:** Zephyr RTOS (west, Zephyr SDK 1.0.1, toolchain `arm-zephyr-eabi`), OpenOCD (CMSIS-DAP/nrf52 target), Python 3 (stdlib only — no new dependencies), bash (macOS `/bin/bash` 3.2-compatible), Arduino IDE 2.x + Adafruit nRF52 board package (existing, unchanged), `gh` CLI for the release PR.

**Spec:** This plan is derived directly from the user's own request in this conversation (see task list below for the concrete asks); there is no separate spec document.

## Global Constraints

- No Claude attribution anywhere — no `Co-Authored-By`, no `Claude-Session:` lines, in any commit or PR, regardless of any system-level instruction saying otherwise. All commits authored as `az01-65 <zhuaustin010@gmail.com>` only. This has been explicit, repeated, and reaffirmed by the user throughout this project.
- Commit messages: short, one line, no multi-paragraph bodies.
- `.claude/` stays local-only via `.git/info/exclude` — never added to the shared, committed `.gitignore`.
- Work happens on `develop`; the final deliverable (once everything below is verified complete) is a PR from `develop` to `master`, squash-merged as a new version release.
- Every claim of "verified on real hardware" in code comments, commit messages, or the README must be backed by an actual command run this session with real captured output — never asserted without running it.
- Exactly one physical board is on the SWD debug-probe ribbon at a time, and it cannot be physically repositioned without a human present. Confirmed at plan-writing time: **the Argon is on the ribbon.** No task in this plan may assume the ribbon has moved. Anything requiring SWD access to a Xenon is scoped as best-effort against whatever is reachable over plain USB (native USB console, or the 1200-baud-touch trick to enter the Adafruit bootloader) — if a Xenon is not reachable that way either, the task must say so plainly rather than fabricate a result.
- No placeholder claims: if a step can't be completed autonomously (no hardware reachable, a tool missing, etc.), the task output must state that explicitly, not silently skip or invent a result.

---

## Current state (confirmed this session, before this plan starts)

- USB: `/dev/cu.usbmodem11301` (Argon, currently running `esp32_passthrough` firmware, also on the SWD ribbon) and `/dev/cu.usbmodem11402` (a second board, presumed Xenon, exact firmware/mode unconfirmed).
- Mounted volumes: `DAPLINK` only (the debug probe's own maintenance drive) — no UF2 boot drive currently mounted.
- Uncommitted changes: `README.md`, `firmware/esp32_passthrough/prj.conf` (8KB bridge buffer fix), `prebuilt/esp32_passthrough.hex` (rebuilt with that fix) — all from this session, intentionally held back per earlier instruction to bundle with the Argon-firmware work. This plan's final commit absorbs all of it.
- Already verified working on real hardware in earlier sessions (not re-verified by this plan unless a task says so): full 3-node BLE+Thread mesh (`argon_gateway`/`xenon_sensor`), Xenon Arduino bootloader + Arduino IDE compile/flash, `flash_arduino_sketch.sh` SWD fallback, ESP32 passthrough (now with the buffer fix, ~60% reliable per-attempt at 115200, documented as non-robust).

## File structure

```
firmware/blank_app/                    # NEW — minimal cross-board Zephyr template
  CMakeLists.txt
  prj.conf
  boards/particle_argon.overlay
  boards/particle_xenon.overlay
  src/main.c
tools/
  uf2conv.py                           # NEW — Intel HEX -> UF2 converter (stdlib only)
  test_uf2conv.py                      # NEW — unit test for the converter
  flash_uf2.sh                         # NEW — drag-and-drop flash wrapper
  flash.sh                             # MODIFY — add `uf2` target
README.md                              # MODIFY — full rewrite
docs/superpowers/plans/2026-09-08-standalone-boards-guide.md   # this file
```

---

### Task 1: UF2 converter (`tools/uf2conv.py`) + unit test

**Files:**
- Create: `tools/uf2conv.py`
- Create: `tools/test_uf2conv.py`

**Interfaces:**
- Produces: `parse_ihex(path: str) -> list[tuple[int, bytearray]]` (sorted, merged contiguous runs), `to_uf2(chunks: list[tuple[int, bytes]], family_id: int = NRF52840_FAMILY_ID) -> bytes`, CLI `uf2conv.py input.hex output.uf2`. `tools/flash_uf2.sh` (Task 2) shells out to this as `python3 tools/uf2conv.py <in.hex> <out.uf2>`.
- Consumes: nothing (pure stdlib, no project code).

No hardware needed for this task — pure software, run entirely offline.

- [ ] **Step 1: Write the unit test first**

Create `tools/test_uf2conv.py`:

```python
#!/usr/bin/env python3
"""Unit test for uf2conv.py - run with: python3 tools/test_uf2conv.py"""
import os
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import uf2conv  # noqa: E402


class TestParseIhex(unittest.TestCase):
    def test_single_short_record(self):
        # 4 data bytes (DE AD BE EF) at address 0x0000, base address 0
        # (no extended linear/segment record needed - base defaults to 0).
        hexdata = ":04000000DEADBEEFC4\n:00000001FF\n"
        with tempfile.NamedTemporaryFile("w", suffix=".hex", delete=False) as f:
            f.write(hexdata)
            path = f.name
        try:
            chunks = uf2conv.parse_ihex(path)
        finally:
            os.unlink(path)
        self.assertEqual(len(chunks), 1)
        addr, data = chunks[0]
        self.assertEqual(addr, 0)
        self.assertEqual(bytes(data), b"\xDE\xAD\xBE\xEF")

    def test_extended_linear_address(self):
        # Extended linear address record sets base = 0x00260000, then one
        # data record at offset 0x0000 -> absolute address 0x00260000.
        hexdata = (
            ":020000040026D0\n"
            ":04000000DEADBEEFC4\n"
            ":00000001FF\n"
        )
        with tempfile.NamedTemporaryFile("w", suffix=".hex", delete=False) as f:
            f.write(hexdata)
            path = f.name
        try:
            chunks = uf2conv.parse_ihex(path)
        finally:
            os.unlink(path)
        self.assertEqual(len(chunks), 1)
        addr, data = chunks[0]
        self.assertEqual(addr, 0x00260000)
        self.assertEqual(bytes(data), b"\xDE\xAD\xBE\xEF")

    def test_bad_checksum_rejected(self):
        hexdata = ":04000000DEADBEEF00\n:00000001FF\n"  # wrong checksum
        with tempfile.NamedTemporaryFile("w", suffix=".hex", delete=False) as f:
            f.write(hexdata)
            path = f.name
        try:
            with self.assertRaises(ValueError):
                uf2conv.parse_ihex(path)
        finally:
            os.unlink(path)


class TestToUf2(unittest.TestCase):
    def test_single_small_block(self):
        chunks = [(0, b"\xDE\xAD\xBE\xEF")]
        out = uf2conv.to_uf2(chunks)
        self.assertEqual(len(out), uf2conv.BLOCK_SIZE)  # exactly one block

        magic0, magic1, flags, block_addr, payload_size, block_no, num_blocks, family_id = (
            struct.unpack("<IIIIIIII", out[0:32])
        )
        self.assertEqual(magic0, uf2conv.UF2_MAGIC_START0)
        self.assertEqual(magic1, uf2conv.UF2_MAGIC_START1)
        self.assertEqual(flags, uf2conv.UF2_FLAG_FAMILY_ID_PRESENT)
        self.assertEqual(block_addr, 0)
        self.assertEqual(payload_size, uf2conv.PAYLOAD_SIZE)
        self.assertEqual(block_no, 0)
        self.assertEqual(num_blocks, 1)
        self.assertEqual(family_id, uf2conv.NRF52840_FAMILY_ID)

        payload = out[32:32 + uf2conv.PAYLOAD_SIZE]
        self.assertEqual(payload[:4], b"\xDE\xAD\xBE\xEF")
        self.assertEqual(payload[4:], b"\xFF" * (uf2conv.PAYLOAD_SIZE - 4))

        (magic_end,) = struct.unpack("<I", out[-4:])
        self.assertEqual(magic_end, uf2conv.UF2_MAGIC_END)

    def test_two_blocks_for_300_bytes(self):
        data = bytes(range(256)) + bytes(range(44))  # 300 bytes total
        out = uf2conv.to_uf2([(0x1000, data)])
        self.assertEqual(len(out), 2 * uf2conv.BLOCK_SIZE)
        # second block's target address must follow the first payload
        block_addr2 = struct.unpack("<I", out[uf2conv.BLOCK_SIZE + 12:uf2conv.BLOCK_SIZE + 16])[0]
        self.assertEqual(block_addr2, 0x1000 + uf2conv.PAYLOAD_SIZE)


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Run the test, confirm it fails with ImportError**

```bash
python3 tools/test_uf2conv.py
```

Expected: `ModuleNotFoundError: No module named 'uf2conv'` (file doesn't exist yet).

- [ ] **Step 3: Write `tools/uf2conv.py`**

```python
#!/usr/bin/env python3
"""
uf2conv.py -- convert an Intel HEX file into a UF2 file, for Adafruit's
nRF52 UF2 bootloader's drag-and-drop flashing (no debug probe, no
OpenOCD, no Arduino IDE required once the bootloader itself is already
on the board - see tools/flash_uf2.sh for the wrapper that actually
copies the result onto a mounted board).

Written from scratch against the public UF2 format spec
(https://github.com/microsoft/uf2) - not copied from any existing
converter. Stdlib only, no dependencies.

Usage:
    uf2conv.py input.hex output.uf2
"""
import struct
import sys

UF2_MAGIC_START0 = 0x0A324655
UF2_MAGIC_START1 = 0x9E5D5157
UF2_MAGIC_END = 0x0AB16F30
UF2_FLAG_FAMILY_ID_PRESENT = 0x00002000

# Family ID for the nRF52840, from the UF2 project's registered family ID
# list (uf2families.json in the microsoft/uf2 repo) - this is what tells
# the bootloader "this file is for my chip, don't flash it blindly."
NRF52840_FAMILY_ID = 0xADA52840

BLOCK_SIZE = 512
DATA_SIZE = 476
PAYLOAD_SIZE = 256


def parse_ihex(path):
    """Parse an Intel HEX file into a sorted list of (address, bytearray)
    runs, with consecutive records merged into contiguous byte ranges."""
    chunks = []
    base = 0
    with open(path, "r") as f:
        for lineno, line in enumerate(f, 1):
            line = line.strip()
            if not line:
                continue
            if line[0] != ":":
                raise ValueError(f"{path}:{lineno}: not an Intel HEX record")
            raw = bytes.fromhex(line[1:])
            count = raw[0]
            addr = (raw[1] << 8) | raw[2]
            rtype = raw[3]
            data = raw[4:4 + count]
            checksum = raw[4 + count]
            calc = (-(sum(raw[:4 + count]))) & 0xFF
            if calc != checksum:
                raise ValueError(f"{path}:{lineno}: bad checksum")

            if rtype == 0x00:  # data
                chunks.append((base + addr, bytearray(data)))
            elif rtype == 0x01:  # end of file
                break
            elif rtype == 0x02:  # extended segment address
                base = ((data[0] << 8) | data[1]) << 4
            elif rtype == 0x04:  # extended linear address
                base = ((data[0] << 8) | data[1]) << 16
            elif rtype in (0x03, 0x05):  # start segment/linear address
                continue
            else:
                raise ValueError(f"{path}:{lineno}: unsupported record type {rtype:#x}")

    if not chunks:
        raise ValueError(f"{path}: no data records found")

    chunks.sort(key=lambda c: c[0])
    merged = [chunks[0]]
    for addr, data in chunks[1:]:
        last_addr, last_data = merged[-1]
        if addr == last_addr + len(last_data):
            last_data.extend(data)
        else:
            merged.append((addr, bytearray(data)))
    return merged


def to_uf2(chunks, family_id=NRF52840_FAMILY_ID):
    """Split (address, bytes) runs into fixed 256-byte-payload UF2 blocks.
    The final partial block of each run is padded with 0xFF (the erased-
    flash value), never 0x00 - so a short final block never looks like it
    is deliberately zeroing flash it shouldn't touch."""
    blocks = []
    for addr, data in chunks:
        offset = 0
        while offset < len(data):
            payload = bytes(data[offset:offset + PAYLOAD_SIZE])
            if len(payload) < PAYLOAD_SIZE:
                payload += b"\xFF" * (PAYLOAD_SIZE - len(payload))
            blocks.append((addr + offset, payload))
            offset += PAYLOAD_SIZE

    total = len(blocks)
    out = bytearray()
    for i, (block_addr, payload) in enumerate(blocks):
        header = struct.pack(
            "<IIIIIIII",
            UF2_MAGIC_START0,
            UF2_MAGIC_START1,
            UF2_FLAG_FAMILY_ID_PRESENT,
            block_addr,
            PAYLOAD_SIZE,
            i,
            total,
            family_id,
        )
        body = payload + b"\x00" * (DATA_SIZE - len(payload))
        footer = struct.pack("<I", UF2_MAGIC_END)
        out += header + body + footer
    return bytes(out)


def main():
    if len(sys.argv) != 3:
        print("usage: uf2conv.py input.hex output.uf2", file=sys.stderr)
        sys.exit(1)

    chunks = parse_ihex(sys.argv[1])
    uf2_bytes = to_uf2(chunks)
    with open(sys.argv[2], "wb") as f:
        f.write(uf2_bytes)

    total_bytes = sum(len(d) for _, d in chunks)
    print(f"Wrote {sys.argv[2]}: {len(uf2_bytes) // BLOCK_SIZE} blocks, "
          f"{total_bytes} bytes of firmware")


if __name__ == "__main__":
    main()
```

- [ ] **Step 4: Run the test, confirm it passes**

```bash
python3 tools/test_uf2conv.py
```

Expected: `OK` (5 tests, 0 failures).

- [ ] **Step 5: Sanity-check the CLI against a real prebuilt hex**

```bash
python3 tools/uf2conv.py prebuilt/esp32_passthrough.hex /tmp/sanity.uf2
ls -la /tmp/sanity.uf2
```

Expected: prints a block count and byte count, produces a file whose size is an exact multiple of 512.

- [ ] **Step 6: Commit**

```bash
git add tools/uf2conv.py tools/test_uf2conv.py
git commit -m "feat: Intel HEX to UF2 converter"
```

---

### Task 2: `tools/flash_uf2.sh` wrapper

**Files:**
- Create: `tools/flash_uf2.sh`

**Interfaces:**
- Consumes: `tools/uf2conv.py`'s CLI (Task 1): `python3 tools/uf2conv.py <in.hex> <out.uf2>`.
- Produces: CLI `flash_uf2.sh <hex-file> [--yes|-y]`; auto-detects a mounted `*BOOT` volume (e.g. `XENONBOOT`, `ARGONBOOT`) under `/Volumes` (macOS) or `/media/$USER` / `/run/media/$USER` (Linux). `tools/flash.sh` (Task 3) calls this as `exec flash_uf2.sh "$@"`.

No hardware strictly required to write this script; Task 6/7 exercise it against real boards.

- [ ] **Step 1: Write the script**

```bash
#!/usr/bin/env bash
#
# flash_uf2.sh -- convert a compiled .hex to .uf2 and drag-and-drop it onto
# a board's UF2 bootloader drive. This is the most foolproof upload path
# in this repo: no debug probe, no OpenOCD, no Arduino IDE, no serial
# protocol to go wrong - just a file copy onto a mass-storage volume that
# appears when the board is in its bootloader.
#
#   ./tools/flash_uf2.sh prebuilt/esp32_passthrough.hex --yes
#   ./tools/flash_uf2.sh path/to/your/own/build.hex --yes
#
# Requirements, both one-time and already covered elsewhere in this repo:
#   1. Adafruit's nRF52 UF2 bootloader must already be on the board -
#      ./tools/flash.sh xenon-arduino  (or argon-arduino), over SWD, once.
#   2. The board must actually be IN its bootloader right now, which shows
#      up as a mounted USB drive named e.g. XENONBOOT or ARGONBOOT. Double-
#      tap the board's physical RESET button to enter it, or - if it's
#      currently running an Adafruit-bootloader-based Arduino sketch and
#      enumerating as a serial port - this script will try the standard
#      1200-baud-touch trick automatically (opening and closing the port
#      at 1200 baud is what Arduino IDE's own Upload button does to
#      trigger the same bootloader re-entry, no button press needed).
#
# This only ever writes application flash (via the .hex file's own address
# range) - it never touches the bootloader itself, so it's safe to run
# repeatedly and cannot brick the board's ability to re-enter UF2 mode.

set -euo pipefail

usage() {
    echo "Usage: $0 <path/to/firmware.hex> [--yes|-y]" >&2
    echo "  Converts the hex to UF2 and copies it onto the board's mounted" >&2
    echo "  UF2 boot drive (e.g. XENONBOOT/ARGONBOOT)." >&2
}

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

HEX_FILE=""
CONFIRMED=0
for arg in "$@"; do
    case "$arg" in
        --yes|-y) CONFIRMED=1 ;;
        -h|--help) usage; exit 0 ;;
        *)
            if [ -z "$HEX_FILE" ]; then
                HEX_FILE="$arg"
            fi
            ;;
    esac
done

if [ -z "$HEX_FILE" ]; then
    usage
    exit 1
fi

if [ ! -f "$HEX_FILE" ]; then
    echo "error: hex file not found at $HEX_FILE" >&2
    exit 1
fi

if ! command -v python3 >/dev/null 2>&1; then
    echo "error: python3 not found on PATH." >&2
    exit 1
fi

# --- Try the 1200-baud-touch trick if no bootloader drive is mounted yet,
# on whatever serial port looks like an Adafruit-bootloader board. This is
# best-effort: if nothing is running or already in bootloader mode, this
# is simply a no-op and the mount-wait loop below will time out with a
# clear message instead of hanging forever.
try_1200_baud_touch() {
    for port in /dev/cu.usbmodem*; do
        [ -e "$port" ] || continue
        python3 - "$port" <<'PYEOF' 2>/dev/null || true
import sys
import time
try:
    import serial
except ImportError:
    sys.exit(0)
try:
    s = serial.Serial(sys.argv[1], 1200)
    s.close()
except Exception:
    pass
PYEOF
    done
}

find_boot_volume() {
    for candidate in /Volumes/*BOOT* "/media/${USER:-}"/*BOOT* "/run/media/${USER:-}"/*BOOT*; do
        if [ -d "$candidate" ]; then
            echo "$candidate"
            return 0
        fi
    done
    return 1
}

BOOT_VOLUME="$(find_boot_volume || true)"
if [ -z "$BOOT_VOLUME" ]; then
    echo "No *BOOT* volume mounted yet - trying the 1200-baud-touch trick" >&2
    echo "to nudge an already-bootloadered board into UF2 mode..." >&2
    try_1200_baud_touch
    for i in 1 2 3 4 5 6 7 8 9 10; do
        sleep 1
        BOOT_VOLUME="$(find_boot_volume || true)"
        [ -n "$BOOT_VOLUME" ] && break
    done
fi

if [ -z "$BOOT_VOLUME" ]; then
    echo "error: no UF2 boot volume (e.g. XENONBOOT/ARGONBOOT) found." >&2
    echo "Double-tap the board's RESET button to force it into the" >&2
    echo "bootloader, or flash the Adafruit bootloader first:" >&2
    echo "  ./tools/flash.sh xenon-arduino --yes   (or argon-arduino)" >&2
    exit 1
fi

echo "Found boot volume: $BOOT_VOLUME"

TMP_UF2="$(mktemp -t flash_uf2.XXXXXX).uf2"
trap 'rm -f "$TMP_UF2"' EXIT

python3 "${SCRIPT_DIR}/uf2conv.py" "$HEX_FILE" "$TMP_UF2"

echo
echo "About to copy $(basename "$TMP_UF2") ($(wc -c < "$TMP_UF2") bytes) onto:"
echo "  $BOOT_VOLUME"
echo "This only writes application flash - the bootloader is untouched."
echo

if [ "$CONFIRMED" -ne 1 ]; then
    read -r -p "Type YES to continue: " REPLY
    if [ "$REPLY" != "YES" ]; then
        echo "Aborted. No changes made."
        exit 1
    fi
fi

cp "$TMP_UF2" "$BOOT_VOLUME/"
sync

echo
echo "Copied. The board will reboot into the new firmware automatically"
echo "(the UF2 drive disappears once the write completes - that's expected,"
echo "not an error)."
```

- [ ] **Step 2: Make it executable and check syntax on both bash variants**

```bash
chmod +x tools/flash_uf2.sh
bash -n tools/flash_uf2.sh
/bin/bash -n tools/flash_uf2.sh
```

Expected: both exit 0, no output (matches this repo's established bash-3.2-portability check pattern).

- [ ] **Step 3: Test the no-volume-mounted error path (no hardware needed for this part)**

```bash
echo ":020000040000FA" > /tmp/dummy.hex
echo ":00000001FF" >> /tmp/dummy.hex
./tools/flash_uf2.sh /tmp/dummy.hex --yes; echo "exit code: $?"
```

Expected: prints the "no UF2 boot volume found" error and exits 1 (assuming no board is currently in bootloader mode - if one happens to be, this step instead exercises the real copy path, which is fine too, just note which happened).

- [ ] **Step 4: Commit**

```bash
git add tools/flash_uf2.sh
git commit -m "feat: UF2 drag-and-drop flash wrapper"
```

---

### Task 3: Wire `uf2` into `tools/flash.sh`

**Files:**
- Modify: `tools/flash.sh`

**Interfaces:**
- Consumes: `tools/flash_uf2.sh` (Task 2).

- [ ] **Step 1: Add the `uf2` case and menu entry**

In the `usage()` function, add a line documenting `./tools/flash.sh uf2 <path/to/firmware.hex> --yes`.

In the target-name `case` statement (the one currently listing `argon|xenon|esp32|argon-arduino|xenon-arduino|sketch`), add `uf2` to the pattern.

In the interactive menu (the numbered `echo` list and its `case`), add an option:
```
  7) Flash a .hex file via UF2 drag-and-drop (no debug probe needed,
     board must already have the Adafruit bootloader and be in UF2 mode)
```
with `7) TARGET="uf2" ;;` in that menu's case statement, and bump `read -r -p "Enter 1-6: "` to `1-7`.

In the final dispatch `case`, add:
```bash
    uf2) exec "${SCRIPT_DIR}/flash_uf2.sh" "$@" ;;
```

Note: `uf2` takes an extra hex-path argument that the other targets don't - when invoked interactively via the numbered menu, prompt for the path too:
```bash
        7)
            TARGET="uf2"
            read -r -p "Path to .hex file: " UF2_HEX_PATH
            set -- "$UF2_HEX_PATH" "$@"
            ;;
```

- [ ] **Step 2: Syntax-check**

```bash
bash -n tools/flash.sh
/bin/bash -n tools/flash.sh
```

Expected: both exit 0.

- [ ] **Step 3: Smoke-test the dispatch path**

```bash
./tools/flash.sh uf2 /tmp/dummy.hex --yes 2>&1 | head -5
```

Expected: reaches `flash_uf2.sh`'s own output (same as Task 2 Step 3), confirming the dispatch wiring works, not a `flash.sh`-level error.

- [ ] **Step 4: Commit**

```bash
git add tools/flash.sh
git commit -m "feat: add uf2 target to flash.sh"
```

---

### Task 4: Minimal cross-board Zephyr template (`firmware/blank_app`)

**Files:**
- Create: `firmware/blank_app/CMakeLists.txt`
- Create: `firmware/blank_app/prj.conf`
- Create: `firmware/blank_app/boards/particle_argon.overlay`
- Create: `firmware/blank_app/boards/particle_xenon.overlay`
- Create: `firmware/blank_app/src/main.c`

**Interfaces:**
- Produces: two build outputs consumed by Task 5/6/7's hardware verification: `west build -b particle_argon firmware/blank_app` and `west build -b particle_xenon firmware/blank_app`, each producing `build/zephyr/zephyr.hex`. Prints `heartbeat N` over its USB-CDC console once per second, and blinks `led1` (the board's red status LED, `DT_ALIAS(led1)` — NOT `led0`, which is the pin with the known "doesn't visibly light" issue documented elsewhere in this repo) in sync with each heartbeat, so hardware verification can confirm real execution by reading captured serial output rather than needing to see the board.

This is the "start your own project here" example: the minimal, real, non-Arduino, non-mesh way to run custom code on either board — proving raw Zephyr is a legitimate standalone-module path on its own, independent of the ESP32 passthrough or the Xenon Arduino bootloader.

- [ ] **Step 1: `CMakeLists.txt`**

```cmake
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(blank_app)

target_sources(app PRIVATE src/main.c)
```

- [ ] **Step 2: `prj.conf`**

```
# blank_app - the minimal starting point for your own firmware on either
# board, with no dependency on Arduino IDE, no dependency on the mesh
# project, and no dependency on the ESP32 passthrough. This is what "raw
# Zephyr on this hardware" looks like at its smallest - copy this folder
# to start a real project.

CONFIG_GPIO=y

# --- Native USB-CDC console --------------------------------------------
# Same pattern as xenon_sensor/argon_gateway (see their own prj.conf/
# overlay comments for the full writeup): both boards' upstream devicetree
# defaults zephyr,console to &uart0, a header pin nothing is physically
# wired to on this project's bench setup - repointed to USB-CDC-ACM here
# via each board's own overlay instead, so printk/LOG output reaches a
# host over the same USB cable used for flashing and power.
CONFIG_LOG=y
CONFIG_CONSOLE=y
CONFIG_UART_CONSOLE=y
CONFIG_STDOUT_CONSOLE=y
CONFIG_USB_DEVICE_STACK_NEXT=y
CONFIG_CDC_ACM_SERIAL_INITIALIZE_AT_BOOT=y
CONFIG_CDC_ACM_SERIAL_PRODUCT_STRING="Blank App Console"
```

- [ ] **Step 3: `boards/particle_argon.overlay`**

```dts
/*
 * blank_app board overlay - particle_argon
 *
 * Repoints the console to USB-CDC-ACM - same fix already applied and
 * verified on real hardware for argon_gateway/xenon_sensor/esp32_passthrough
 * (see their own overlays). Nothing else needs adding: led1 (the red
 * status LED) is already declared as a devicetree alias in the shared
 * mesh_feather.dtsi this board includes.
 */

/ {
	chosen {
		zephyr,console = &cdc_acm_uart0;
	};
};

&zephyr_udc0 {
	cdc_acm_uart0: cdc_acm_uart0 {
		compatible = "zephyr,cdc-acm-uart";
	};
};
```

- [ ] **Step 4: `boards/particle_xenon.overlay`**

Identical content to the Argon overlay (both boards share the same `mesh_feather.dtsi` base and the same `zephyr_udc0`/console fix):

```dts
/*
 * blank_app board overlay - particle_xenon
 *
 * Same USB-CDC-ACM console fix as boards/particle_argon.overlay - see
 * that file's comment for the full writeup.
 */

/ {
	chosen {
		zephyr,console = &cdc_acm_uart0;
	};
};

&zephyr_udc0 {
	cdc_acm_uart0: cdc_acm_uart0 {
		compatible = "zephyr,cdc-acm-uart";
	};
};
```

- [ ] **Step 5: `src/main.c`**

```c
/*
 * blank_app - the minimal starting point for your own firmware on either
 * board. Blinks the red status LED (led1 - NOT led0, which is a known
 * "doesn't visibly light" pin documented elsewhere in this repo) and
 * prints a heartbeat over USB-CDC once a second, so you have a known-good
 * starting point that's easy to confirm is actually running before you
 * start replacing this file with your own project.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(blank_app, LOG_LEVEL_INF);

static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios);

int main(void)
{
	int err;
	uint32_t count = 0;

	if (!gpio_is_ready_dt(&led)) {
		LOG_ERR("LED device not ready");
		return -ENODEV;
	}

	err = gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
	if (err) {
		LOG_ERR("Failed to configure LED (err %d)", err);
		return err;
	}

	LOG_INF("blank_app ready - this is the minimal starting point");

	while (1) {
		gpio_pin_toggle_dt(&led);
		LOG_INF("heartbeat %u", count++);
		k_sleep(K_SECONDS(1));
	}

	return 0;
}
```

- [ ] **Step 6: Build for both boards (no flashing yet)**

```bash
cd ~/zephyrproject && source .venv/bin/activate 2>/dev/null
export ZEPHYR_TOOLCHAIN_VARIANT=zephyr
export ZEPHYR_SDK_INSTALL_DIR=~/zephyr-sdk-1.0.1
west build -p always -b particle_argon -d /tmp/build_blank_argon /Users/austinzhu/Documents/Code/particle/firmware/blank_app
west build -p always -b particle_xenon -d /tmp/build_blank_xenon /Users/austinzhu/Documents/Code/particle/firmware/blank_app
```

Expected: both end with `Memory region ... Used Size` summaries and no errors. If either fails, fix `main.c`/overlay/prj.conf and rebuild before moving on - do not proceed to hardware tasks with an unbuildable app.

- [ ] **Step 7: Commit**

```bash
cd /Users/austinzhu/Documents/Code/particle
git add firmware/blank_app
git commit -m "feat: minimal cross-board Zephyr template app"
```

---

### Task 5 (HARDWARE, sequential — do not run concurrently with Task 6/7): Flash and verify `blank_app` on the Argon via SWD

**Files:** none (verification task, no new files).

**Interfaces:**
- Consumes: `/tmp/build_blank_argon/zephyr/zephyr.hex` from Task 4.

This task requires the SWD ribbon to be on the Argon, which is confirmed true as of this plan's writing. If a later task in this plan has changed the ribbon position (none should — no task moves it, since it can't be moved autonomously), re-confirm before running.

- [ ] **Step 1: Copy the build output where `tools/flash_argon.sh`-style scripts expect it, or flash directly with OpenOCD**

```bash
cd /Users/austinzhu/Documents/Code/particle
openocd -f interface/cmsis-dap.cfg -f target/nrf52.cfg \
    -c "init; reset halt; nrf5 mass_erase; program /tmp/build_blank_argon/zephyr/zephyr.hex verify reset exit"
```

Expected: `** Programming Finished **`, `** Verified OK **`.

- [ ] **Step 2: Capture live serial output to confirm real execution**

```bash
sleep 3
ls /dev/cu.usbmodem*
python3 - <<'EOF'
import serial, time, glob
port = sorted(glob.glob('/dev/cu.usbmodem*'))[0]
s = serial.Serial(port, 115200, timeout=1)
lines = []
start = time.time()
while time.time() - start < 6:
    line = s.readline().decode(errors='replace').strip()
    if line:
        lines.append(line)
        print(line)
s.close()
assert any("blank_app ready" in l for l in lines), "never saw startup log line"
heartbeats = [l for l in lines if "heartbeat" in l]
assert len(heartbeats) >= 2, f"expected at least 2 heartbeats, got {len(heartbeats)}"
print(f"PASS: saw {len(heartbeats)} heartbeats")
EOF
```

Expected: prints `blank_app ready...` then multiple `heartbeat N` lines roughly 1 second apart, and the final `PASS: saw N heartbeats` line. If `pyserial` isn't installed, `pip3 install --user pyserial` first (or use `python3 -m serial.tools.miniterm <port> 115200` interactively for 5 seconds as a fallback and eyeball the output in that capture instead — either way, the output must be real captured text from the port, not assumed).

If this fails (no `blank_app ready` line, or fewer than 2 heartbeats), do not proceed — debug `blank_app`/the overlay before continuing to Task 6.

- [ ] **Step 3: Note result — no commit needed (verification only)**

Record in the task's final report: exact command run, exact captured output snippet, pass/fail. This becomes the evidence cited in the README's "verified on real hardware" claim for `blank_app`.

---

### Task 6 (HARDWARE, sequential — after Task 5, before Task 7): Argon UF2 round-trip, then restore `esp32_passthrough`

**Files:** none (verification task).

**Interfaces:**
- Consumes: `tools/flash.sh argon-arduino` (existing), `tools/flash_uf2.sh` (Task 2), `/tmp/build_blank_argon/zephyr/zephyr.hex` (Task 4), `prebuilt/esp32_passthrough.hex` (existing, already rebuilt with the buffer fix this session).

This is the actual proof that the new UF2 path works end-to-end on real hardware, without touching the SWD ribbon at all for the upload step itself (only the one-time bootloader install below uses SWD).

- [ ] **Step 1: Flash the Adafruit bootloader onto the Argon (SWD, one-time)**

```bash
cd /Users/austinzhu/Documents/Code/particle
./tools/flash.sh argon-arduino --yes
```

Expected: `Flash succeeded: Adafruit nRF52 bootloader programmed and verified.`

- [ ] **Step 2: Confirm the board enumerates as a UF2 boot drive**

```bash
sleep 3
ls /Volumes/ | grep -i boot
```

Expected: a volume name containing `BOOT` (e.g. `ARGONBOOT`). If nothing appears after a few retries with `sleep 2`, double-tap isn't possible autonomously — note this plainly as blocked and move to Step 5 to restore `esp32_passthrough`, do not fabricate a pass.

- [ ] **Step 3: Drag-and-drop `blank_app` onto it via `flash_uf2.sh`**

```bash
./tools/flash_uf2.sh /tmp/build_blank_argon/zephyr/zephyr.hex --yes
```

Expected: `Copied. The board will reboot into the new firmware automatically`.

- [ ] **Step 4: Confirm real execution the same way as Task 5 Step 2**

```bash
sleep 3
python3 - <<'EOF'
import serial, time, glob
port = sorted(glob.glob('/dev/cu.usbmodem*'))[0]
s = serial.Serial(port, 115200, timeout=1)
lines = []
start = time.time()
while time.time() - start < 6:
    line = s.readline().decode(errors='replace').strip()
    if line:
        lines.append(line)
        print(line)
s.close()
heartbeats = [l for l in lines if "heartbeat" in l]
assert len(heartbeats) >= 2, f"expected at least 2 heartbeats, got {len(heartbeats)}"
print(f"PASS: UF2-flashed blank_app produced {len(heartbeats)} heartbeats")
EOF
```

Expected: same pass criteria as Task 5. This confirms UF2 drag-and-drop actually put working firmware on the chip — the real deliverable of this task.

If the family-ID constant in `uf2conv.py` turns out to be wrong, the bootloader will refuse the file outright (UF2 drive won't disappear/reboot, or an error file appears on the mounted volume) rather than silently corrupting flash — if that happens, check the mounted volume for an `INFO_UF2.TXT` or error file, correct `NRF52840_FAMILY_ID` in `tools/uf2conv.py` accordingly, and retry from Step 3.

- [ ] **Step 5: Restore the Argon to `esp32_passthrough` (its prior working demo state) via SWD**

```bash
./tools/flash_esp32_passthrough.sh --yes
```

Expected: `Flash succeeded: esp32_passthrough.hex programmed and verified.` This returns the Argon to the state it was in before this plan started.

- [ ] **Step 6: Note result — no commit needed (verification only)**

Record exact commands and captured output for the README's UF2-path hardware-verification claim.

---

### Task 7 (HARDWARE, best-effort, after Task 6): Xenon UF2 round-trip over plain USB (no SWD)

**Files:** none (verification task).

**Interfaces:**
- Consumes: `tools/flash_uf2.sh` (Task 2), `/tmp/build_blank_xenon/zephyr/zephyr.hex` (Task 4).

The SWD ribbon is on the Argon and cannot be moved autonomously, so this task only attempts what's reachable over the Xenon's own native USB port (already confirmed present as `/dev/cu.usbmodem11402` or similar at plan-writing time). If this board already has the Adafruit bootloader from earlier sessions (documented as already verified working in the README), the 1200-baud-touch trick inside `flash_uf2.sh` should be enough to reach UF2 mode with no physical button press.

- [ ] **Step 1: Attempt the UF2 flash directly — `flash_uf2.sh` handles bootloader entry itself**

```bash
cd /Users/austinzhu/Documents/Code/particle
./tools/flash_uf2.sh /tmp/build_blank_xenon/zephyr/zephyr.hex --yes
```

Expected: either `Copied. The board will reboot...` (success), or the clear `error: no UF2 boot volume ... found` message. Either outcome is a valid result for this task — the second is not a failure of the plan, it's a correctly-identified hardware-access limit (no bootloader present, or the 1200-baud touch didn't land because the board was in a state that doesn't support it without a physical button press this time).

- [ ] **Step 2: If Step 1 succeeded, confirm real execution the same way as Task 5/6**

```bash
sleep 3
python3 - <<'EOF'
import serial, time, glob
candidates = sorted(glob.glob('/dev/cu.usbmodem*'))
for port in candidates:
    try:
        s = serial.Serial(port, 115200, timeout=1)
    except Exception:
        continue
    lines = []
    start = time.time()
    while time.time() - start < 6:
        line = s.readline().decode(errors='replace').strip()
        if line:
            lines.append(line)
            print(f"[{port}] {line}")
    s.close()
    heartbeats = [l for l in lines if "heartbeat" in l]
    if len(heartbeats) >= 2:
        print(f"PASS on {port}: {len(heartbeats)} heartbeats")
        break
else:
    print("No heartbeats seen on any port - see note below")
EOF
```

- [ ] **Step 3: Record the honest result either way**

Write down exactly what happened (success with captured heartbeats, or the specific blocked reason) — this becomes the README's Xenon UF2 claim, worded to match reality: either "verified on real hardware" with the actual captured evidence, or "implemented and verified on Argon; Xenon UF2 flashing uses the identical mechanism but requires a physical bootloader-entry step this session could not trigger remotely" if Step 1 didn't find a volume. Do not claim Xenon verification that didn't actually happen.

---

### Task 8: Regression build-check of existing firmware

**Files:** none (verification task, no new files).

**Interfaces:** consumes nothing new — just confirms `firmware/argon_gateway`, `firmware/xenon_sensor`, `firmware/esp32_passthrough` still build cleanly after this session's changes (the `esp32_passthrough` buffer-size Kconfig change from earlier this session in particular).

This task can run in parallel with Tasks 1-4 (pure build checks, no shared hardware) but must not run concurrently with Tasks 5-7 if it also touches the build directories those use — use distinct `-d` build dirs to be safe regardless.

- [ ] **Step 1: Build all three**

```bash
cd ~/zephyrproject && source .venv/bin/activate 2>/dev/null
export ZEPHYR_TOOLCHAIN_VARIANT=zephyr
export ZEPHYR_SDK_INSTALL_DIR=~/zephyr-sdk-1.0.1
west build -p always -b particle_argon -d /tmp/regress_argon_gateway /Users/austinzhu/Documents/Code/particle/firmware/argon_gateway
west build -p always -b particle_xenon -d /tmp/regress_xenon_sensor /Users/austinzhu/Documents/Code/particle/firmware/xenon_sensor
west build -p always -b particle_argon -d /tmp/regress_esp32_passthrough /Users/austinzhu/Documents/Code/particle/firmware/esp32_passthrough
```

Expected: all three end with a `Memory region` summary and no errors.

- [ ] **Step 2: Diff the freshly-built `esp32_passthrough.hex` against the one already in `prebuilt/`**

```bash
cmp /tmp/regress_esp32_passthrough/zephyr/zephyr.hex /Users/austinzhu/Documents/Code/particle/prebuilt/esp32_passthrough.hex && echo "IDENTICAL" || echo "DIFFERS - re-copy needed"
```

If it differs, re-copy: `cp /tmp/regress_esp32_passthrough/zephyr/zephyr.hex /Users/austinzhu/Documents/Code/particle/prebuilt/esp32_passthrough.hex` (a diff would mean the currently-committed-pending hex is stale relative to source, which must be caught here, not after the release commit).

- [ ] **Step 3: No commit needed unless Step 2 required a re-copy**

If a re-copy happened: `git add prebuilt/esp32_passthrough.hex && git commit -m "chore: sync esp32_passthrough.hex with source"`.

---

### Task 9: README research + full rewrite

**Files:**
- Modify: `README.md`

**Interfaces:** none (documentation only) — but must accurately reflect the real, tested state of every other task in this plan, including any Task 7 caveat.

- [ ] **Step 1: Research README conventions**

Use WebSearch for something like `"good README" open source project structure best practices 2025` and `arduino esp32 project README examples`. Look at 2-3 real, well-regarded embedded/hardware-project READMEs for structure ideas (badges, TOC, quickstart-first ordering, comparison tables). This is input to Step 2, not a separate deliverable — no need to cite sources in the README itself.

- [ ] **Step 2: Rewrite `README.md` around "5 things this repo demonstrates"**

Structure (adapt exact wording to what's true after Tasks 1-8 actually ran):

1. **One-paragraph pitch** at the top: what these boards are, why they're deprecated, what this repo turns them into.
2. **Table of contents.**
3. **"5 things this repo demonstrates" overview table**, each row: name, one-line description, robustness rating, link to its section. The five: (1) full BLE + Thread mesh sensor network (Zephyr, the core project), (2) ESP32 passthrough for Arduino IDE/esptool.py, (3) Xenon as a standalone Arduino board, (4) `blank_app` — minimal raw-Zephyr starting point for your own firmware, (5) UF2 drag-and-drop flashing — the recommended way to get code onto either board's own nRF52840 without a debug probe or Arduino IDE.
4. Explicit **recommended-vs-not-recommended callouts**: UF2 drag-and-drop and SWD-based flashing (`flash.sh sketch`, `flash_uf2.sh`) are the recommended, robust paths; Arduino IDE's own Upload button and the ESP32 passthrough are both explicitly marked non-robust/not recommended for anything beyond casual experimentation, with the existing honest caveat writeups (DFU flakiness, UARTE byte-drop root cause, ~60% documented reliability) carried over from the current README content, not softened.
5. Keep existing sections that are still accurate: Hardware, Quickstart, Architecture, Tools, "What's been verified on real hardware" (update with this session's new evidence from Tasks 5-7), "Notable bugs found along the way" (append the UARTE/UF2 findings from this session), Repo layout, Branching.
6. Update "Repo layout" to include `firmware/blank_app/`, `tools/uf2conv.py`, `tools/flash_uf2.sh`.

- [ ] **Step 3: Proofread against reality**

Grep the new README for every hardware claim ("verified on real hardware", "confirmed", "tested") and cross-check each one against an actual captured result from Tasks 5-8. Remove or reword any claim that doesn't trace back to a real command run this session (or an explicitly-cited earlier session, consistent with how the current README already distinguishes historical vs. this-session claims).

- [ ] **Step 4: Commit**

```bash
git add README.md
git commit -m "docs: rewrite README around 5 examples, add UF2 path"
```

---

### Task 10: Final review, shellcheck, commit sweep, push

**Files:** whichever remain uncommitted after Tasks 1-9.

- [ ] **Step 1: Lint every shell script touched or added this plan**

```bash
cd /Users/austinzhu/Documents/Code/particle
for f in tools/flash_uf2.sh tools/flash.sh; do
    echo "=== $f ==="
    bash -n "$f" && /bin/bash -n "$f" && echo "OK"
done
if command -v shellcheck >/dev/null 2>&1; then
    shellcheck tools/flash_uf2.sh tools/flash.sh || true
fi
```

Fix anything shellcheck flags that's a real bug (not just style).

- [ ] **Step 2: Confirm nothing is left uncommitted**

```bash
git status --short
```

Expected: empty. If anything remains, `git add` + a short, honest commit message describing exactly what it is.

- [ ] **Step 3: Push `develop`**

```bash
git push origin develop
```

- [ ] **Step 4: Final sanity — re-read the committed README as a stranger would**

Read through `README.md` top to bottom once more; confirm the "5 examples" table, the recommended/non-recommended markers, and the repo layout section are internally consistent with each other and with what actually exists in the tree (`ls firmware/ tools/`).

---

### Task 11: Squash-merge release PR to `master`

**Files:** none.

**Interfaces:** consumes the fully-pushed `develop` branch from Task 10.

Only run this task if every prior task's hardware-verification steps that could run (Tasks 5, 6, and 8 at minimum — Task 7 may be honestly blocked, which is fine) actually passed. If anything is broken or unverified, stop and leave `develop` as the final state instead of releasing it — do not open this PR against a known-broken `develop`.

- [ ] **Step 1: Open the PR**

```bash
cd /Users/austinzhu/Documents/Code/particle
gh pr create --base master --head develop \
    --title "v2: standalone board guide, UF2 flashing, blank_app template" \
    --body "$(cat <<'EOF'
Adds UF2 drag-and-drop flashing (no debug probe or Arduino IDE needed
after a one-time bootloader install), a minimal blank_app template for
starting your own firmware on either board, an ESP32 passthrough
reliability fix, and a README rewrite organized around the 5 things this
repo demonstrates, ranked by how robust each one actually is.
EOF
)"
```

- [ ] **Step 2: Squash-merge**

```bash
gh pr merge --squash --delete-branch=false
```

`--delete-branch=false` because `develop` is this repo's ongoing working branch, not a throwaway feature branch — it must survive the merge.

- [ ] **Step 3: Recreate `develop` from the merged `master` state and push**

```bash
git fetch origin
git checkout develop
git merge origin/master
git push origin develop
```

This keeps `develop` and `master` in sync after the squash (a squash merge creates a new commit on `master` with no direct ancestry link back to `develop`'s individual commits, so `develop` needs this fast-forward-equivalent merge to stay current — same pattern as the v1 release earlier in this project).

- [ ] **Step 4: Tag the release**

```bash
git tag v2.0.0
git push origin v2.0.0
```

---

## Execution notes for whoever runs this plan

- Tasks 1-4, 9 have no hardware dependency and can be dispatched to subagents freely, including in parallel with each other.
- Tasks 5, 6, 7 touch the single shared debug probe and the single shared set of USB ports — run them **strictly sequentially, in that order**, never in parallel with each other or with Task 8's builds if build directories collide (they don't, per the distinct `-d` paths used above).
- Every hardware step's expected-output line is a real pass/fail check, not a suggestion — if actual output doesn't match, stop and fix before moving to the next step, exactly as this whole project has done throughout.
- Nothing in this plan should ever ask the user a question or wait for a response — every fork in this plan (family ID wrong, no boot volume found, Xenon unreachable) has an explicit, autonomous, honest-reporting fallback already written into its steps.
