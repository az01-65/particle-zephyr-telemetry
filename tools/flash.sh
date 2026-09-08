#!/usr/bin/env bash
#
# flash.sh -- single entry point for flashing whichever board is currently
# on the debug probe. Thin wrapper around flash_argon.sh/flash_xenon.sh
# (which still work standalone) - this just saves remembering which
# per-board script to call.
#
#   ./tools/flash.sh argon --yes
#   ./tools/flash.sh xenon --yes
#   ./tools/flash.sh              # no board named -> asks interactively
#
# Anything after the board name (e.g. --yes/-y) is passed straight through
# to the underlying per-board script - see those scripts for what mass
# erase + flash actually does and why it always resets to Mode 1 (BLE).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

usage() {
    echo "Usage: $0 [argon|xenon] [--yes|-y]" >&2
    echo "  With no board named, asks interactively which one is on the debug probe." >&2
}

TARGET=""
if [ $# -gt 0 ]; then
    case "$1" in
        argon|xenon)
            TARGET="$1"
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        --yes|-y)
            # First arg is a flag, not a board name - fall through to the
            # interactive prompt below with the flag still in "$@".
            ;;
        *)
            echo "error: unrecognized board '$1'" >&2
            usage
            exit 1
            ;;
    esac
fi

if [ -z "$TARGET" ]; then
    echo "Which board is currently on the debug probe (SWD ribbon)?"
    echo "  1) Argon"
    echo "  2) Xenon"
    read -r -p "Enter 1 or 2: " choice
    case "$choice" in
        1) TARGET="argon" ;;
        2) TARGET="xenon" ;;
        *)
            echo "Aborted: not a valid choice." >&2
            exit 1
            ;;
    esac
fi

case "$TARGET" in
    argon) exec "${SCRIPT_DIR}/flash_argon.sh" "$@" ;;
    xenon) exec "${SCRIPT_DIR}/flash_xenon.sh" "$@" ;;
esac
