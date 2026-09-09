#!/usr/bin/env bash
#
# flash_arduino_bootloader.sh -- flash Adafruit's official nRF52 UF2/DFU
# bootloader onto a directly connected CMSIS-DAP debug probe (e.g. Particle
# Debugger), using raw OpenOCD. No Zephyr toolchain, west, or pyocd required
# -- just OpenOCD:
#
#   brew install openocd
#   ./tools/flash_arduino_bootloader.sh xenon --yes
#   ./tools/flash_arduino_bootloader.sh argon --yes   # see the CAVEAT below first
#
# What this actually does, and why it's here: unlike the ESP32 passthrough
# (firmware/esp32_passthrough), this doesn't bridge to a second chip -- the
# Argon and Xenon's own nRF52840 already has native USB. Adafruit's
# MIT-licensed bootloader project (github.com/adafruit/Adafruit_nRF52_Bootloader)
# has first-party board support for both "Particle Argon" and "Particle
# Xenon" at the BOOTLOADER level (see src/boards/particle_argon/ and
# src/boards/particle_xenon/ in that repo). Once flashed, the chip's own
# USB port speaks the standard nRF52 UF2/DFU protocol, the same one every
# Adafruit nRF52 board uses.
#
# CAVEAT, Argon specifically (read before using `argon` here): Adafruit's
# separate Arduino CORE project (github.com/adafruit/Adafruit_nRF52_Arduino,
# what Arduino IDE's "Adafruit nRF52 boards" package actually installs) has
# NEVER included a "Particle Argon" board definition - confirmed directly
# against that repo's own boards.txt, which lists "Particle Xenon" but no
# Argon entry at all, in any version. Flashing this bootloader onto an
# Argon leaves it as a genuine, correctly-enumerating UF2/DFU target, but
# there is currently no way to select it in Arduino IDE or compile a sketch
# for it through the official tooling - the bootloader alone isn't useful
# without a matching board definition. Closing that gap would mean writing
# a custom Arduino variant (pin/board files) for the Argon, which does not
# exist yet in this project. Xenon has no such gap - see below.
#
# Xenon: fully working, verified on real hardware (compile in Arduino IDE,
# select "Particle Xenon", Upload). One real caveat there too: `LED_BUILTIN`
# (Arduino digital pin 7 / nRF52840 pin P1.12) is a known, Adafruit-acknowledged
# "wontfix" bug on this specific board - the pin toggles correctly (confirmed
# by reading the raw GPIO register live over SWD, mid-blink) but doesn't
# visibly light anything, most likely because that particular LED isn't
# populated on this board revision. Use the onboard RGB status LED instead -
# Arduino digital pins 22/23/24 (red/green/blue, nRF52840 P0.13/14/15) - the
# same physical LED this project's own xenon_sensor Zephyr firmware already
# drives and was verified working on real hardware many times over.
#
# The prebuilt/particle_xenon_adafruit_bootloader.hex vendored here is NOT
# Adafruit's standalone GitHub release build - it's copied byte-for-byte
# from INSIDE the Arduino Boards Manager package itself
# (Adafruit nRF52 boards v1.7.0's own bootloader/particle_xenon/
# particle_xenon_bootloader-0.9.1_s140_6.1.1.hex). This distinction matters:
# an earlier version of this script vendored the standalone release build
# (v0.11.0) instead, which enumerates and mounts as a UF2 drive just fine,
# but made every Arduino IDE upload fail (the DFU transfer would start,
# then the port would vanish and never return) - root-caused on real
# hardware to a version mismatch between that bootloader and the DFU
# tooling Arduino IDE's installed board package actually ships, not a bug
# in this project's own flashing. Using the package's own bundled
# bootloader instead resolved it. If you install a different version of
# the "Adafruit nRF52 boards" package than 1.7.0, re-extract its bundled
# bootloader from the same relative path rather than assuming this exact
# file still matches.
#
# Known remaining instability, even with the correct bootloader version:
# Arduino IDE's Upload button uses Adafruit's own DFU-over-serial tool
# (adafruit-nrfutil), which has a long-standing, widely-reported bug on
# macOS with native-USB nRF52840 boards (see e.g.
# github.com/adafruit/Adafruit_nRF52_Arduino issues #224, #366, #405) where
# an upload can succeed once and then fail on the next attempt with the
# same "port vanished" symptom - confirmed happening on this exact board
# even after fixing the version mismatch above. If Upload fails:
#   1. Try again - it is genuinely intermittent, not a permanent state.
#   2. Manually double-tap the board's physical RESET button right before
#      clicking Upload, forcing it into bootloader mode yourself rather
#      than relying on the IDE's automatic 1200bps-touch reset - this
#      mitigates but does not guarantee-fix the underlying tool bug.
#   3. Guaranteed-reliable fallback: compile in Arduino IDE (Verify, not
#      Upload), then flash the resulting .hex directly over SWD -
#      tools/flash_arduino_sketch.sh automates exactly that.
#
# This mass-erases the board before programming, which wipes whatever
# firmware/bootloader is currently on it (the mesh firmware, esp32_passthrough,
# or anything else). That step requires explicit confirmation (either pass
# --yes/-y, or confirm interactively when asked).
#
# This is a one-way door in the context of this repo's other firmware: once
# a board is running the Adafruit bootloader plus whatever Arduino sketch you
# upload to it, getting back to the mesh demo means reflashing it with
# ./tools/flash_argon.sh or ./tools/flash_xenon.sh, same as switching between
# any of this repo's other firmware images.

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
    echo "Usage: $0 <argon|xenon> [--yes|-y]" >&2
}

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [ $# -eq 0 ]; then
    usage
    exit 1
fi

BOARD="$1"
shift

case "$BOARD" in
    argon)
        HEX_FILE="${SCRIPT_DIR}/../prebuilt/particle_argon_adafruit_bootloader.hex"
        BOARD_TITLE="Argon"
        VERSION_NOTE="v0.11.0 (Adafruit's standalone GitHub release build - there is no Arduino-package-bundled version to match, since no Arduino package includes Argon support at all; see the CAVEAT in this script's header comment)"
        ;;
    xenon)
        HEX_FILE="${SCRIPT_DIR}/../prebuilt/particle_xenon_adafruit_bootloader.hex"
        BOARD_TITLE="Xenon"
        VERSION_NOTE="v0.9.1 (the exact build bundled inside Arduino Boards Manager's 'Adafruit nRF52 boards' v1.7.0 package - required for Upload to work; see this script's header comment)"
        ;;
    *)
        echo "error: unrecognized board '${BOARD}' (expected argon or xenon)" >&2
        usage
        exit 1
        ;;
esac

CONFIRMED=0
for arg in "$@"; do
    case "$arg" in
        --yes|-y)
            CONFIRMED=1
            ;;
    esac
done

if ! command -v openocd >/dev/null 2>&1; then
    echo "error: openocd not found on PATH." >&2
    echo "Install it with:  brew install openocd" >&2
    exit 1
fi

if [ ! -f "${HEX_FILE}" ]; then
    echo "error: hex file not found at ${HEX_FILE}" >&2
    exit 1
fi

echo "About to flash: ${HEX_FILE}"
echo "(Adafruit's nRF52 bootloader for the Particle ${BOARD_TITLE}, ${VERSION_NOTE})"
if [ "${BOARD}" = "argon" ]; then
    echo
    echo "CAVEAT: Arduino IDE currently has no 'Particle Argon' board to select"
    echo "or compile for - this flashes a working UF2/DFU bootloader, but there is"
    echo "nothing to upload to it yet through official tooling. See this script's"
    echo "header comment for the full explanation."
fi
echo
echo "This will MASS ERASE the board first, wiping its current firmware"
echo "and bootloader (any of this repo's own firmware, if that's what's on it"
echo "now). This step is irreversible."
echo

if [ "${CONFIRMED}" -ne 1 ]; then
    read -r -p "Type YES to continue: " REPLY
    if [ "${REPLY}" != "YES" ]; then
        echo "Aborted. No changes made."
        exit 1
    fi
fi

trap print_permission_hint ERR

echo "Mass erasing..."
openocd -f interface/cmsis-dap.cfg -f target/nrf52.cfg -c "init; reset halt; nrf5 mass_erase; exit"

echo "Programming ${HEX_FILE}..."
openocd -f interface/cmsis-dap.cfg -f target/nrf52.cfg -c "program ${HEX_FILE} verify reset exit"

trap - ERR

echo
echo "Flash succeeded: Adafruit nRF52 bootloader programmed and verified."
if [ "${BOARD}" = "xenon" ]; then
    echo "This board's own chip is now a standalone Arduino target. In Arduino IDE:"
    echo "  1. Boards Manager -> install 'Adafruit nRF52 boards'"
    echo "  2. Select 'Particle Xenon' as the board"
    echo "  3. Upload sketches directly over this same USB cable"
    echo "  (If Upload fails with a vanished-port error, see this script's header"
    echo "  comment - it's a known, intermittent upstream bug, not a bad flash.)"
else
    echo "This board now has a working UF2/DFU bootloader, but Arduino IDE has no"
    echo "'Particle Argon' board definition to pair it with yet - see this script's"
    echo "header comment (CAVEAT section) for the full explanation."
fi
