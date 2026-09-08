# Particle Gen 3 Mesh Sensor Network

A BLE + Thread mesh sensor network built on Zephyr RTOS, running on three
deprecated Particle "Gen 3" boards (1 Argon, 2 Xenons) whose original
Particle Device OS backend was EOL'd. Same nRF52840 silicon, same hardware,
new firmware stack.

Clone it, flash it, and it just works — no Zephyr toolchain required to run
it, only to rebuild it from source.

## Fastest thing to try: use the Argon's ESP32 with Arduino IDE

The Argon has a second chip most people never touch: an onboard ESP32
Wi-Fi co-processor, normally locked to Particle's own AT-command firmware.
This repo includes a standalone firmware image,
[`firmware/esp32_passthrough`](firmware/esp32_passthrough), that turns the
Argon's existing USB port into a direct, transparent bridge to that ESP32 —
once it's flashed, Arduino IDE or `esptool.py` can flash and use the ESP32
exactly like any normal, directly-USB-attached ESP32 dev board. No wiring,
no separate USB-serial adapter, no manual boot-button presses.

```bash
brew install openocd                          # or your OS's package manager
./tools/flash.sh esp32 --yes                  # with the Argon on the debug probe

# Then just point Arduino IDE (ESP32 board package) or esptool.py at
# whatever port the Argon enumerates as, same as any other ESP32 board:
pip install esptool
esptool.py --port /dev/cu.usbmodemXXXX chip_id
```

This is a separate, standalone firmware image — it has nothing to do with
the mesh project below, and flashing it replaces whatever else is on the
Argon's nRF52840 (the mesh gateway, if that's what's there). Flash
`./tools/flash.sh argon --yes` to put the mesh gateway firmware back.

How it actually works: the ESP32 has no USB of its own, only a UART link to
the nRF52840 plus two GPIO control lines. This firmware bridges the raw
UART traffic (via Zephyr's own in-tree `zephyr,uart-bridge` driver) and
translates the host's DTR/RTS serial control signals into the ESP32's
GPIO0/EN pins, using the exact mapping a normal ESP32 dev board's
USB-serial chip uses — which is what lets an unmodified `esptool.py`/Arduino
IDE upload flow reset the chip into its bootloader automatically. Verified
on real hardware: `esptool.py chip_id` over this bridge correctly detects
the real onboard ESP32-D0WD and reads back its real MAC address. See
[`firmware/esp32_passthrough/src/main.c`](firmware/esp32_passthrough/src/main.c)
for the full mechanism and exactly where the GPIO pin numbers come from.

### Also possible: turn the Xenon itself into a standalone Arduino board

Separately from the ESP32 passthrough above, a Xenon's *own* nRF52840 can
become a real, directly-selectable Arduino board — no bridging needed,
since (unlike the ESP32) it already has native USB. This uses Adafruit's
official, MIT-licensed nRF52 bootloader and Arduino core, which has
first-party "Particle Xenon" support built in.

```bash
./tools/flash.sh xenon-arduino --yes    # with a Xenon on the debug probe
```

Then in Arduino IDE: add `https://adafruit.github.io/arduino-board-index/package_adafruit_index.json`
under Settings → Additional boards manager URLs, install "Adafruit nRF52"
from Boards Manager, and select **Particle Xenon** as the board. From
there it's a normal Arduino board — same USB cable, no separate adapter.

**Two real caveats, both verified on real hardware, worth knowing before you start:**

- **`LED_BUILTIN` doesn't visibly work** — a known, Adafruit-acknowledged
  "wontfix" bug specific to this board (confirmed: the pin genuinely
  toggles correctly, checked by reading the raw GPIO register live over
  SWD mid-blink, it just doesn't light anything visible, most likely
  because that particular LED isn't populated on this board revision).
  Use the onboard RGB LED instead — Arduino pins **22 / 23 / 24** (red /
  green / blue), the same physical LED this project's own Zephyr firmware
  already drives successfully.
- **Arduino IDE's Upload button is intermittently broken** — a
  long-standing upstream bug in Adafruit's DFU-over-serial tool on macOS
  with native-USB nRF52840 boards (multiple multi-year-old GitHub issues,
  not anything specific to this repo). It can succeed once and then fail
  on the very next attempt with the port simply vanishing. If it happens:
  retry, or manually double-tap RESET right before clicking Upload. The
  reliable fallback that always works: compile in Arduino IDE (Verify, not
  Upload), then `./tools/flash.sh sketch --yes` — it finds your most
  recently compiled sketch and flashes it directly over SWD, bypassing the
  broken tool entirely.

The Argon's own nRF52840 doesn't have this path yet — Adafruit's Arduino
core has never included a "Particle Argon" board definition (confirmed
directly against their `boards.txt`), so there's no board to select even
though a working bootloader can be flashed (`./tools/flash.sh argon-arduino`).
Closing that gap means writing a custom Arduino board variant, which
doesn't exist in this project yet.

## What this actually demonstrates

- A real-time-OS firmware architecture with two independent, runtime-switchable
  radio stacks sharing one 2.4GHz radio (BLE and Thread/802.15.4), switched by
  a physical button gesture with no reflash needed.
- A genuine 3-node Thread mesh: one FTD Leader (Argon) relaying live sensor
  telemetry from two MTD Children (Xenons) over IPv6 UDP multicast — verified
  on real hardware, not simulated.
- The debugging trail behind it is real too: several non-obvious bugs (radio
  IRQ-sharing livelocks, a kernel panic from calling a blocking socket API in
  the wrong thread context, and a genuinely obscure Zephyr networking gap)
  were root-caused with SWD ground-truth — reading live memory and OpenThread's
  own internal state directly off the chip — rather than guessed at. See
  [Notable bugs found along the way](#notable-bugs-found-along-the-way).

## Hardware

| Board | Role | Radio capability |
|---|---|---|
| Particle Argon | Gateway | nRF52840 (BLE + 802.15.4) + ESP32 Wi-Fi co-processor (unused by the mesh firmware — see `firmware/esp32_passthrough` above to use it directly) |
| Particle Xenon ×2 | Sensor nodes | nRF52840 (BLE + 802.15.4) |

Both are Adafruit Feather-form-factor nRF52840 boards. See
[docs/ARGON_XENON_SETUP_GUIDE.md](docs/ARGON_XENON_SETUP_GUIDE.md) for board
identification, DFU/JTAG basics, and general background if you're starting
from bare, unfamiliar hardware.

## Quickstart

### Path A: just want to see it run (no toolchain)

Needs only [OpenOCD](https://openocd.org/) and Python 3.

```bash
brew install openocd                    # or your OS's package manager

# One board on the SWD debug probe at a time:
./tools/flash.sh argon --yes            # with the Argon on the probe
./tools/flash.sh xenon --yes            # repeat per Xenon

python3 -m venv .venv && source .venv/bin/activate
pip install -r tools/requirements.txt

ls /dev/cu.usbmodem*                    # find the Argon's console port
python3 tools/mesh_visualizer.py /dev/cu.usbmodemXXXX
```

Every board boots into **Mode 1 (BLE)** by default — that's what a fresh
flash always resets it to, deterministically (see
[tools/flash_argon.sh](tools/flash_argon.sh)'s header comment for why). The
visualizer will show one live Xenon node immediately, no button presses
needed.

To see the actual **mesh** (Mode 2, Thread): hold the MODE button on each
board until its LED flashes 4 times in its current color, then the target
color — release while showing the target color to commit the switch, or
release early to cancel and stay put. Order doesn't matter (MTD Xenons
retry their attach automatically), but expect 20 seconds to a minute for
each to actually join. Once at least two boards are attached, the
visualizer fills in each node live.

### Path B: build from source

See [docs/TOOLCHAIN.md](docs/TOOLCHAIN.md) for the full west/Zephyr SDK
bring-up (T2 out-of-tree topology — the Zephyr workspace lives outside this
repo). Once set up:

```bash
cd ~/zephyrproject && source .venv/bin/activate
west build -b particle_argon -d /tmp/build_argon  /path/to/this/repo/firmware/argon_gateway
west build -b particle_xenon -d /tmp/build_xenon  /path/to/this/repo/firmware/xenon_sensor
```

## Architecture

Both `firmware/argon_gateway` and `firmware/xenon_sensor` are dual-mode
apps: both radio stacks are compiled into the same image, but only one is
ever actually started at runtime, gated by a single persisted mode byte in
NVS flash storage. Rebooting is required to switch, since the nRF52840 has
one radio and Zephyr's BLE controller and 802.15.4 driver both want the
same `RADIO_IRQn` vector — see the `CONFIG_DYNAMIC_INTERRUPTS` /
`CONFIG_BT_CTLR_DYNAMIC_INTERRUPTS` comment block in either `prj.conf` for
the actual mechanism (this was the first real bug hunted down in this
project — see below).

| | Mode 1 (BLE) | Mode 2 (Thread) |
|---|---|---|
| **Xenon** | GATT peripheral, advertises `"XenonSensor"`, notifies subscribers | 802.15.4 MTD — attaches as a Child only, **never** Router/Leader eligible (structural, not a workaround — see below) |
| **Argon** | GATT central — scans, connects, subscribes to one peripheral at a time | 802.15.4 FTD — forms/leads the mesh, relays multicast telemetry from every attached Child |
| Topology | One-to-one (by design — `bt_le_scan_stop()` fires on the first match) | One gateway, many children (the actual mesh) |

Both apps share one wire format and one relay path: whichever mode
produced a sample, the Argon decodes it and prints the exact same JSON line
on its USB serial console —

```json
{"node_id": "0x45", "temp_c": 23.50, "vdd_mv": 3300, "seq": 12}
```

— so nothing downstream (`tools/telemetry_monitor.py`,
`tools/mesh_visualizer.py`) needs to know or care which radio produced a
given line. `node_id` is derived from each Xenon's own factory-programmed
hardware ID (`hwinfo_get_device_id()`), folded to one byte — every Xenon
runs the byte-identical firmware image, so this is what makes two physical
boards distinguishable on one console without a per-board build.

### Protocols, side by side

**BLE (Mode 1)** — a custom 128-bit GATT service:

```
Service:        a3f8c2d0-6b1e-4a7f-9c3d-8e2b5f1a9d40
Characteristic: a3f8c2d1-6b1e-4a7f-9c3d-8e2b5f1a9d40  (notify)
```

**Thread (Mode 2)** — UDP over a realm-local IPv6 multicast group,
`ff03::abcd` port `4242`. Not one of Thread's own reserved groups
(`ff03::1`/`ff03::2`), so a listener has to explicitly subscribe. Hardcoded
demo network credentials (not a secret — no real commissioning flow is
implemented, by design, per this project's scope):

```
PAN ID:            0xFEED
Extended PAN ID:   58:45:4e:4f:4e:00:00:01
Network key:       de:ad:be:ef:ca:fe:f0:0d:13:37:c0:de:00:01:02:03
Channel:           15 (2425 MHz)
Network name:      XenonMesh
```

Both directions share one 9-byte packed struct on the wire:

```c
struct __packed telemetry_payload {
	int16_t  temp_centi_c;  /* 0.01 degC steps */
	uint16_t vdd_mv;
	uint32_t seq;
	uint8_t  node_id;
};
```

### Xenons are structurally incapable of leading the mesh

This is a real architectural choice, not a naming detail.
`CONFIG_OPENTHREAD_MTD=y` on the Xenons means their Thread stack has no
code path to ever become a Router or Leader — they can only attach as a
Child. Confirmed on real hardware that leaving this at the FTD default let
a Xenon and the Argon each independently self-promote to Leader of their
own separate partition before ever hearing each other, silently splitting
the mesh into two disconnected halves even though every node shared
identical network credentials. MTD closes off that entire failure class,
rather than working around one instance of it.

## Tools

| Tool | What it's for |
|---|---|
| `tools/flash.sh` | One entry point for flashing whichever board/image is on the probe (`./tools/flash.sh argon\|xenon\|esp32\|xenon-arduino\|argon-arduino\|sketch --yes`). Thin wrapper — the per-image scripts below still work standalone. |
| `tools/flash_argon.sh` / `tools/flash_xenon.sh` / `tools/flash_esp32_passthrough.sh` | Mass-erase + program this repo's own mesh/passthrough firmware from `prebuilt/*.hex` via OpenOCD. No west/Zephyr SDK needed. |
| `tools/flash_arduino_bootloader.sh` | Mass-erase + program Adafruit's official Arduino bootloader (`argon\|xenon`) — see the caveats above and this script's own header comment. |
| `tools/flash_arduino_sketch.sh` | Flash an already-compiled Arduino sketch straight over SWD — the reliable fallback for Arduino IDE's own flaky Upload button. Auto-detects the most recently compiled sketch by default. |
| `tools/telemetry_monitor.py` | Scrolling log of decoded JSON telemetry lines. |
| `tools/mesh_visualizer.py` | Live redrawing dashboard — mesh topology tree + per-node table, LIVE/STALE inferred from packet timing. Works identically for BLE or Thread telemetry. |

All flashing always resets the persisted mode back to Mode 1 (BLE) — a
full chip erase is what makes "freshly flashed = definitely BLE" a
guarantee rather than a coin flip (a partial flash can leave NVS mid-write
in an unpredictable state). See either flash script's header comment for
the full reasoning.

## What's been verified on real hardware

- **BLE mode**: Argon central connects to a Xenon peripheral, subscribes,
  and relays live temp/VDD notifications — confirmed via both the JSON
  console output and the visualizer.
- **Thread mesh, full 3-node**: Argon (Leader) plus both Xenons (MTD
  Children) confirmed on one shared partition (matching Thread partition
  ID, read directly over SWD off all three boards) with real telemetry
  from both Xenons arriving at the gateway and correctly attributed by
  `node_id` — not simulated, not assumed from LED color.
- **Mode switching**: the interactive hold-to-confirm gesture, and that a
  fresh flash deterministically lands every board back in Mode 1.
- **Radio coexistence**: BLE and Thread stacks compiled into the same
  image without the IRQ-sharing conflict that originally made this
  impossible to build at all (see below).

### Disclosed gaps

- BLE central connects to exactly one peripheral at a time — original,
  intentional scope; not something the Thread work was meant to extend.
- No real Thread commissioning/joiner flow — every node auto-joins via
  hardcoded credentials, appropriate for a demo, not a real deployment.
- The Argon's Thread mode is a node that relays one multicast group, not a
  full Thread Border Router (no external IPv6 routing/NAT64).
- No automated test suite — every check in this project's history was a
  real, physically-verified hardware test (SWD reads, live serial capture),
  not a simulation or unit test harness.
- Arduino IDE's own Upload button, for a Xenon running Adafruit's
  bootloader, is intermittently broken by a known upstream bug (see
  "Also possible" above) — genuinely flaky, not something this repo can
  fully fix, though a guaranteed-reliable fallback (`flash_arduino_sketch.sh`)
  is provided.
- `LED_BUILTIN` doesn't visibly work on a Xenon running Adafruit's
  bootloader — a separate, Adafruit-acknowledged "wontfix" bug, not this
  repo's Zephyr firmware (which drives the RGB LED correctly).
- The Argon has no equivalent standalone-Arduino path for its own chip —
  Adafruit's Arduino core has never included a "Particle Argon" board
  definition. Only the Xenon has this capability today.

## Notable bugs found along the way

Kept short on purpose — full root-cause writeups live as comments at each
fix site in the source, not duplicated here.

1. **BLE/Thread radio coexistence livelock** — both stacks statically bound
   the same `RADIO_IRQn` vector at build/boot time; fixed with dynamic
   interrupt attachment so only the active stack ever claims it.
2. **Split mesh partitions** — Xenons self-promoting to Leader before ever
   hearing the Argon. Fixed by making Xenons MTD (see above), not by adding
   startup-order logic.
3. **Kernel panic from the "obvious" fix** — moving a multicast-group join
   to fire the instant Thread attach completes seemed straightforward, but
   calling it directly from OpenThread's own state-change callback context
   panicked the kernel. Fixed by moving the actual (blocking) socket call
   into the receive thread's own context, which exists for exactly that
   purpose.
4. **Multicast telemetry silently never arriving, at all** — the hardest
   one. Every layer *looked* correct (matching Thread partitions, sender's
   `zsock_sendto()` genuinely succeeding, the receive thread genuinely
   alive and blocked in `recvfrom()`) and it still didn't work. Root cause
   turned out to be two stacked gaps: `CONFIG_NET_MGMT_EVENT` wasn't
   enabled, so the code that hands a socket-level multicast join over to
   OpenThread's own core didn't even compile in; and even after that,
   Zephyr's `net_ipv6_mld_join()` leaves an address on
   `NET_IF_IPV6_NO_MLD` interfaces (which OpenThread's interface correctly
   sets) permanently marked "used" but never "joined" — a real gap in
   Zephyr itself, confirmed by directly inspecting live `struct
   net_if_ipv6` memory over SWD, byte by byte, against the real ELF's
   struct layout. Worked around by finishing the join Zephyr's own call
   left incomplete.
5. **Arduino bootloader/tooling version mismatch** — flashing Adafruit's
   *standalone GitHub release* bootloader onto a Xenon made every Arduino
   IDE upload fail, port vanishing mid-transfer, even though the bootloader
   itself worked fine on its own (correct USB enumeration, mounted its UF2
   drive correctly). Root cause: the *Arduino Boards Manager package*
   bundles its own, different bootloader version (v0.9.1) paired with its
   own upload tool — using the mismatched standalone release (v0.11.0)
   broke the DFU handshake between the two. Fixed by vendoring the
   bootloader from *inside* the installed Arduino package instead of
   Adafruit's separate GitHub releases.

## Repo layout

```
firmware/
  argon_gateway/      Gateway firmware (BLE central / Thread FTD)
  xenon_sensor/       Sensor node firmware (BLE peripheral / Thread MTD)
  esp32_passthrough/  Standalone: USB<->UART bridge to the Argon's own ESP32
prebuilt/            Ready-to-flash .hex files, regenerated per commit
tools/                Flashing scripts + host-side Python utilities
docs/
  TOOLCHAIN.md                        Zephyr SDK / west bring-up notes
  ARGON_XENON_SETUP_GUIDE.md          Board identification, DFU/JTAG basics
  Particle_Development_Kit_Instructions.md   Beginner-friendly path (Arduino/CircuitPython vs. Zephyr)
```

## Branching

`master` holds tagged releases; all active development happens on
`develop` and gets squashed in.
