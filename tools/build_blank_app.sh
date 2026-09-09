#!/usr/bin/env bash
#
# build_blank_app.sh -- build firmware/blank_app for one board, in either
# of its two variants:
#
#   ./tools/build_blank_app.sh argon           # direct-SWD build, flash 0x0
#   ./tools/build_blank_app.sh xenon --uf2     # UF2/bootloader-safe, flash 0x26000
#
# Prints the resulting .hex path on success. This exists because the UF2
# variant's build command is easy to get subtly, silently wrong: it needs
# TWO extra flags (both -DEXTRA_DTC_OVERLAY_FILE and -DEXTRA_CONF_FILE -
# the overlay alone is not enough, since CONFIG_USE_DT_CODE_PARTITION
# defaults off and the devicetree chosen property alone is then ignored),
# and both must be ABSOLUTE paths (Zephyr's build system does not
# absolutize a relative -DEXTRA_* path before handing it to the DTS
# preprocessor, which runs from a different working directory - a
# relative path here fails outright, it does not silently work).
#
# Requires the Zephyr workspace at ~/zephyrproject (west, Zephyr SDK at
# ~/zephyr-sdk-1.0.1) - see docs/TOOLCHAIN.md for the full bring-up.

set -euo pipefail

usage() {
    echo "Usage: $0 <argon|xenon> [--uf2]" >&2
    echo "  --uf2   build the bootloader-safe (flash 0x26000) variant instead" >&2
    echo "          of the default direct-SWD-flash (flash 0x0) variant" >&2
}

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if [ $# -eq 0 ]; then
    usage
    exit 1
fi

BOARD_ARG="$1"
shift

UF2=0
for arg in "$@"; do
    case "$arg" in
        --uf2) UF2=1 ;;
        -h|--help) usage; exit 0 ;;
    esac
done

case "$BOARD_ARG" in
    argon) BOARD="particle_argon" ;;
    xenon) BOARD="particle_xenon" ;;
    *)
        echo "error: unrecognized board '${BOARD_ARG}' (expected argon or xenon)" >&2
        usage
        exit 1
        ;;
esac

if [ ! -d "$HOME/zephyrproject" ]; then
    echo "error: ~/zephyrproject not found - see docs/TOOLCHAIN.md for setup." >&2
    exit 1
fi

EXTRA_ARGS=()
if [ "$UF2" -eq 1 ]; then
    BUILD_DIR="/tmp/build_blank_${BOARD_ARG}_uf2"
    OVERLAY="${REPO_ROOT}/firmware/blank_app/boards/${BOARD}_uf2.overlay"
    CONF="${REPO_ROOT}/firmware/blank_app/boards/${BOARD}_uf2.conf"
    if [ ! -f "$OVERLAY" ] || [ ! -f "$CONF" ]; then
        echo "error: expected UF2 overlay/conf fragments not found:" >&2
        echo "  $OVERLAY" >&2
        echo "  $CONF" >&2
        exit 1
    fi
    EXTRA_ARGS=(-- "-DEXTRA_DTC_OVERLAY_FILE=${OVERLAY}" "-DEXTRA_CONF_FILE=${CONF}")
else
    BUILD_DIR="/tmp/build_blank_${BOARD_ARG}"
fi

cd "$HOME/zephyrproject"
source .venv/bin/activate 2>/dev/null || true
export ZEPHYR_TOOLCHAIN_VARIANT=zephyr
export ZEPHYR_SDK_INSTALL_DIR="$HOME/zephyr-sdk-1.0.1"

west build -p always -b "$BOARD" -d "$BUILD_DIR" \
    "${REPO_ROOT}/firmware/blank_app" "${EXTRA_ARGS[@]}"

HEX="${BUILD_DIR}/zephyr/zephyr.hex"
if [ ! -f "$HEX" ]; then
    echo "error: build succeeded but expected hex not found at $HEX" >&2
    exit 1
fi

echo
echo "Built: $HEX"
if [ "$UF2" -eq 1 ]; then
    echo "(UF2/bootloader-safe variant, flash 0x26000 - use with ./tools/flash_uf2.sh)"
else
    echo "(direct-SWD variant, flash 0x0 - use with a raw 'openocd ... program' after a mass erase)"
fi
