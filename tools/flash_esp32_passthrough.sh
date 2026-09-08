#!/usr/bin/env bash
#
# flash_esp32_passthrough.sh -- flash a prebuilt esp32_passthrough .hex onto
# a directly connected CMSIS-DAP debug probe (e.g. Particle Debugger), using
# raw OpenOCD. No Zephyr toolchain, west, or pyocd required -- just OpenOCD:
#
#   brew install openocd
#   ./tools/flash_esp32_passthrough.sh --yes
#
# This is a SEPARATE firmware image from argon_gateway, not a mode of it --
# flashing this REPLACES whatever's currently on the Argon's nRF52840
# (the mesh gateway, if that's what's there). Once flashed, the Argon's
# existing USB port becomes a transparent bridge to its own onboard ESP32,
# so Arduino IDE / esptool.py can flash and use it directly, exactly like
# any normal, directly-USB-attached ESP32 dev board. Flash argon_gateway
# back onto the nRF52840 (./tools/flash_argon.sh) to return the Argon to
# the mesh demo.
#
# This mass-erases the board before programming, which wipes whatever
# firmware/bootloader is currently on it. That step requires explicit
# confirmation (either pass --yes/-y, or confirm interactively when asked).

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

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
HEX_FILE="${SCRIPT_DIR}/../prebuilt/esp32_passthrough.hex"

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
    echo "Make sure a prebuilt esp32_passthrough.hex has been placed in prebuilt/." >&2
    exit 1
fi

echo "About to flash: ${HEX_FILE}"
echo "This will MASS ERASE the board first, wiping its current firmware"
echo "and bootloader (including argon_gateway, if that's what's on it now)."
echo "This step is irreversible."
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
echo "Flash succeeded: esp32_passthrough.hex programmed and verified."
echo "The Argon's USB port is now a direct bridge to its onboard ESP32 --"
echo "point Arduino IDE or esptool.py at it like any normal ESP32 board."
