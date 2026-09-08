# Toolchain Setup

Records the exact Zephyr toolchain setup used to build and flash the firmware in `firmware/`, and confirms it was verified end-to-end against real hardware before any application code was written.

## Versions confirmed working (macOS, Apple Silicon)

- `west` 1.5.0
- `pyocd` 0.45.1
- Zephyr SDK 1.0.1 (includes `arm-zephyr-eabi`, needed for the nRF52840 on both boards)
- Python 3.14.6 (Homebrew) — no compatibility fallback was needed; earlier Zephyr guidance flags known pip build issues on 3.12/3.13 for this setup, but none occurred here.

## Setup steps

```bash
brew install cmake ninja gperf python-tk ccache qemu dtc libmagic wget openocd

python3 -m venv ~/zephyrproject/.venv
source ~/zephyrproject/.venv/bin/activate
pip install west
west init -m https://github.com/zephyrproject-rtos/zephyr ~/zephyrproject
cd ~/zephyrproject && west update
west packages pip --install   # also pulls in pyocd
west zephyr-export
cd ~/zephyrproject/zephyr && west sdk install
```

The Zephyr source tree and SDK live in `~/zephyrproject` and `~/zephyr-sdk-1.0.1` respectively — **outside this repository**, per standard Zephyr "T2 out-of-tree application" practice. Only the application code in `firmware/`, host tooling in `tools/`, and prebuilt binaries in `prebuilt/` are committed here.

## Flashing tool notes

`particle_argon` and `particle_xenon` register `pyocd`, `nrfjprog`, `jlink`, and `nrfutil` as their Zephyr west runners — **not** `openocd`. So:

- **Development loop** (`west flash`): uses `pyocd`, which talks natively to the CMSIS-DAP-class probe (the official Particle Debugger accessory is CMSIS-DAP based, not a J-Link).
- **Zero-toolchain end-user path** (`tools/flash_argon.sh` / `tools/flash_xenon.sh`): uses raw `openocd` directly against a prebuilt `.hex`, since it only needs `brew install openocd` — no Python/west/SDK required at all.

## macOS-specific gotcha (did not occur here, but worth knowing)

OpenOCD/pyocd's CMSIS-DAP backend uses `hidapi` for USB-HID access. On modern macOS this can require the terminal app to have "Input Monitoring" permission under System Settings → Privacy & Security — without it, the probe can fail to enumerate in a way that looks like a hardware fault rather than a permissions issue. Not hit during this setup, but flagged here in case it comes up on a different machine.

## Verification performed

- `west build -p always -b particle_argon -d /tmp/blinky_test zephyr/samples/basic/blinky` — built cleanly (FLASH 1.93% used, RAM 1.65% used).
- `west flash -d /tmp/blinky_test` — flashed the physical, USB/SWD-connected Argon via pyocd + the CMSIS-DAP probe.
- Re-running the flash reported the on-chip content as byte-for-byte identical to the built image, confirming the program-and-verify round trip actually works, not just that the tool exited 0.
