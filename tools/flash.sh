#!/usr/bin/env bash
#
# flash.sh -- single entry point for flashing whichever board is currently
# on the debug probe. Thin wrapper around the individual flash_*.sh scripts
# (which still work standalone) - this just saves remembering which one to
# call.
#
#   ./tools/flash.sh argon --yes
#   ./tools/flash.sh xenon --yes
#   ./tools/flash.sh esp32 --yes           # esp32_passthrough - see that script's header
#   ./tools/flash.sh xenon-arduino --yes   # Adafruit bootloader for the Xenon's own chip
#   ./tools/flash.sh argon-arduino --yes   # see CAVEAT in flash_arduino_bootloader.sh's header
#   ./tools/flash.sh sketch --yes          # flash an already-compiled Arduino sketch over SWD
#   ./tools/flash.sh                       # no target named -> asks interactively
#
# Anything after the target name (e.g. --yes/-y) is passed straight through
# to the underlying script - see each one's own header comment for what
# mass erase + flash actually does.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

usage() {
    echo "Usage: $0 [argon|xenon|esp32|xenon-arduino|argon-arduino|sketch|uf2] [--yes|-y]" >&2
    echo "  ./tools/flash.sh uf2 <path/to/firmware.hex> --yes" >&2
    echo "  With no target named, asks interactively which one is on the debug probe." >&2
}

TARGET=""
if [ $# -gt 0 ]; then
    case "$1" in
        argon|xenon|esp32|argon-arduino|xenon-arduino|sketch|uf2)
            TARGET="$1"
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        --yes|-y)
            # First arg is a flag, not a target name - fall through to the
            # interactive prompt below with the flag still in "$@".
            ;;
        *)
            echo "error: unrecognized target '$1'" >&2
            usage
            exit 1
            ;;
    esac
fi

if [ -z "$TARGET" ]; then
    echo "Which board/firmware do you want to flash?"
    echo "  1) Argon - mesh gateway firmware"
    echo "  2) Xenon - mesh sensor firmware"
    echo "  3) Argon - ESP32 passthrough (use its onboard ESP32 with Arduino"
    echo "     IDE/esptool.py)"
    echo "  4) Xenon - Arduino bootloader (use the Xenon's OWN chip as a"
    echo "     standalone Arduino board - fully working, verified)"
    echo "  5) Argon - Arduino bootloader (Argon's own chip - CAVEAT: Arduino"
    echo "     IDE has no board definition for it yet, see the script header)"
    echo "  6) Flash an already-compiled Arduino sketch over SWD (reliable"
    echo "     fallback for Arduino IDE's own flaky Upload button)"
    echo "  7) Flash a .hex file via UF2 drag-and-drop (no debug probe needed,"
    echo "     board must already have the Adafruit bootloader and be in UF2 mode)"
    read -r -p "Enter 1-7: " choice
    case "$choice" in
        1) TARGET="argon" ;;
        2) TARGET="xenon" ;;
        3) TARGET="esp32" ;;
        4) TARGET="xenon-arduino" ;;
        5) TARGET="argon-arduino" ;;
        6) TARGET="sketch" ;;
        7)
            TARGET="uf2"
            read -r -p "Path to .hex file: " UF2_HEX_PATH
            set -- "$UF2_HEX_PATH" "$@"
            ;;
        *)
            echo "Aborted: not a valid choice." >&2
            exit 1
            ;;
    esac
fi

case "$TARGET" in
    argon) exec "${SCRIPT_DIR}/flash_argon.sh" "$@" ;;
    xenon) exec "${SCRIPT_DIR}/flash_xenon.sh" "$@" ;;
    esp32) exec "${SCRIPT_DIR}/flash_esp32_passthrough.sh" "$@" ;;
    argon-arduino) exec "${SCRIPT_DIR}/flash_arduino_bootloader.sh" argon "$@" ;;
    xenon-arduino) exec "${SCRIPT_DIR}/flash_arduino_bootloader.sh" xenon "$@" ;;
    sketch) exec "${SCRIPT_DIR}/flash_arduino_sketch.sh" "$@" ;;
    uf2) exec "${SCRIPT_DIR}/flash_uf2.sh" "$@" ;;
esac
