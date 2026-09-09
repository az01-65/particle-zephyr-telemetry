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

if cp "$TMP_UF2" "$BOOT_VOLUME/"; then
    sync
    echo
    echo "Copied. The board will reboot into the new firmware automatically"
    echo "(the UF2 drive disappears once the write completes - that's expected,"
    echo "not an error)."
else
    # The board reboots out of mass-storage mode as soon as the UF2 write
    # completes, which can happen mid-cp and make cp report an I/O error.
    # That's the expected, benign race described above - only treat it as
    # a real failure if the boot volume is still there.
    if [ -d "$BOOT_VOLUME" ]; then
        echo "error: failed to copy $(basename "$TMP_UF2") onto $BOOT_VOLUME" >&2
        exit 1
    fi
    sync || true
    echo
    echo "cp reported an error, but $BOOT_VOLUME is already gone - that's the"
    echo "expected reboot-mid-write race, not a real failure. The board has"
    echo "already rebooted into the new firmware. Treating this as success."
fi
