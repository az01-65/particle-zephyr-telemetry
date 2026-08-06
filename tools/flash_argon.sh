#!/usr/bin/env bash
#
# flash_argon.sh -- flash a prebuilt Argon gateway .hex onto a directly
# connected CMSIS-DAP debug probe (e.g. Particle Debugger), using raw
# OpenOCD. No Zephyr toolchain, west, or pyocd required -- just OpenOCD:
#
#   brew install openocd
#   ./tools/flash_argon.sh --yes
#
# This mass-erases the board before programming, which wipes whatever
# firmware/bootloader is currently on it. That step requires explicit
# confirmation (either pass --yes/-y, or confirm interactively when asked).
#
# Side effect, worth knowing: this also always resets the persisted BLE/
# Thread mode selection (see src/main.c) back to its Mode 1 (BLE) default,
# since mass-erase wipes the NVS storage partition along with everything
# else. This is intentional, not a bug -- a partial (non-erasing) flash can
# leave the persisted mode in an unpredictable state if NVS happened to be
# mid-garbage-collection at the moment of reset, so a full erase is what
# makes "freshly flashed = definitely Mode 1 (BLE)" a reliable guarantee
# rather than a coin flip.

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
HEX_FILE="${SCRIPT_DIR}/../prebuilt/argon_gateway.hex"

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
    echo "Make sure a prebuilt argon_gateway.hex has been placed in prebuilt/." >&2
    exit 1
fi

echo "About to flash: ${HEX_FILE}"
echo "This will MASS ERASE the board first, wiping its current firmware"
echo "and bootloader. This step is irreversible."
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
echo "Flash succeeded: argon_gateway.hex programmed and verified."
