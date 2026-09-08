#!/usr/bin/env bash
#
# flash_arduino_sketch.sh -- flash a sketch you've already compiled in
# Arduino IDE directly over SWD, bypassing Arduino IDE's own Upload button.
#
#   1. In Arduino IDE: Sketch -> Verify/Compile (NOT Upload)
#   2. ./tools/flash_arduino_sketch.sh --yes
#
# Why this exists: Arduino IDE's Upload button for Adafruit nRF52 boards
# (including Particle Xenon) goes through Adafruit's own DFU-over-serial
# tool, which has a long-standing, widely-reported upstream bug on macOS
# with native-USB nRF52840 boards - an upload can succeed once and then
# fail on the very next attempt with the port simply vanishing and never
# coming back (see tools/flash_arduino_bootloader.sh's header comment for
# the full writeup and GitHub issue references). This script sidesteps
# that tool entirely: Arduino IDE still compiles the sketch normally and
# leaves a plain, valid Intel HEX file sitting in its build cache; this
# just finds the most recently compiled one and programs it straight over
# SWD via OpenOCD, the same reliable mechanism this whole project already
# uses for its own Zephyr firmware.
#
# This does NOT mass-erase and does NOT touch the bootloader - it only
# programs the address range the compiled .hex file itself specifies
# (which Adafruit's linker scripts place well above the SoftDevice and
# well below the bootloader), so the Arduino bootloader you flashed with
# flash_arduino_bootloader.sh stays intact. Safe to run repeatedly.
#
# By default this auto-detects the most recently compiled sketch across
# ALL of Arduino IDE's build cache - it does not know or care which sketch
# you meant, only which one you compiled most recently. Pass an explicit
# path as the first argument to flash a specific .hex file instead:
#
#   ./tools/flash_arduino_sketch.sh /path/to/YourSketch.ino.hex --yes

set -euo pipefail

print_permission_hint() {
    echo >&2
    echo "error: openocd failed (see output above)." >&2
    echo "Common cause on macOS: the terminal app running this script needs" >&2
    echo "'Input Monitoring' permission for OpenOCD's CMSIS-DAP backend to see" >&2
    echo "the USB HID probe. Grant it under System Settings -> Privacy & Security" >&2
    echo "-> Input Monitoring, then quit and reopen your terminal and retry." >&2
    echo "A missing permission can otherwise look like a generic 'no device found'" >&2
    echo "or 'unable to open CMSIS-DAP device' hardware error." >&2
}

usage() {
    echo "Usage: $0 [path/to/Sketch.ino.hex] [--yes|-y]" >&2
    echo "  With no path given, auto-detects the most recently compiled sketch" >&2
    echo "  in Arduino IDE's own build cache." >&2
}

# Arduino IDE's build cache location varies by OS; check the common ones.
CACHE_DIRS=(
    "$HOME/Library/Caches/arduino/sketches"    # macOS
    "$HOME/.cache/arduino/sketches"            # Linux
)

HEX_FILE=""
CONFIRMED=0
for arg in "$@"; do
    case "$arg" in
        --yes|-y)
            CONFIRMED=1
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            if [ -z "$HEX_FILE" ]; then
                HEX_FILE="$arg"
            fi
            ;;
    esac
done

if [ -z "$HEX_FILE" ]; then
    echo "No path given - searching Arduino IDE's build cache for the most" >&2
    echo "recently compiled sketch..." >&2

    NEWEST=""
    NEWEST_MTIME=0
    for cache_dir in "${CACHE_DIRS[@]}"; do
        [ -d "$cache_dir" ] || continue
        while IFS= read -r f; do
            # Skip the "with_bootloader" combined images - we only want the
            # plain application hex, not bootloader+SoftDevice+app together.
            case "$f" in
                *with_bootloader*) continue ;;
            esac
            mtime=$(stat -f "%m" "$f" 2>/dev/null || stat -c "%Y" "$f" 2>/dev/null)
            if [ -n "$mtime" ] && [ "$mtime" -gt "$NEWEST_MTIME" ]; then
                NEWEST_MTIME="$mtime"
                NEWEST="$f"
            fi
        done < <(find "$cache_dir" -name "*.ino.hex" 2>/dev/null)
    done

    if [ -z "$NEWEST" ]; then
        echo "error: no compiled sketch found in any of:" >&2
        for cache_dir in "${CACHE_DIRS[@]}"; do
            echo "  $cache_dir" >&2
        done
        echo "Compile a sketch in Arduino IDE first (Sketch -> Verify/Compile)," >&2
        echo "or pass an explicit .hex path as the first argument." >&2
        exit 1
    fi

    HEX_FILE="$NEWEST"
fi

if ! command -v openocd >/dev/null 2>&1; then
    echo "error: openocd not found on PATH." >&2
    echo "Install it with:  brew install openocd" >&2
    exit 1
fi

if [ ! -f "${HEX_FILE}" ]; then
    echo "error: hex file not found at ${HEX_FILE}" >&2
    exit 1
fi

HEX_MTIME=$(stat -f "%Sm" "${HEX_FILE}" 2>/dev/null || stat -c "%y" "${HEX_FILE}" 2>/dev/null || echo "unknown")

echo "About to flash: ${HEX_FILE}"
echo "(compiled: ${HEX_MTIME})"
echo "This programs only the application flash region - the bootloader is"
echo "left untouched. Not a mass erase."
echo

if [ "${CONFIRMED}" -ne 1 ]; then
    read -r -p "Type YES to continue: " REPLY
    if [ "${REPLY}" != "YES" ]; then
        echo "Aborted. No changes made."
        exit 1
    fi
fi

trap print_permission_hint ERR

echo "Programming ${HEX_FILE}..."
openocd -f interface/cmsis-dap.cfg -f target/nrf52.cfg \
    -c "init; reset halt; program ${HEX_FILE} verify reset exit"

trap - ERR

echo
echo "Flash succeeded: sketch programmed and verified."
