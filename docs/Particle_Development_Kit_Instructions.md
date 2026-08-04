# Particle Development Kit Instructions

Working notes and setup instructions for the Particle Argon and Xenon development boards (Gen 3 "Particle Mesh" hardware, both now discontinued by Particle).

---

## 1. Introduction: The Boards and Their Stock Firmware

### 1.1 What these boards are

Both boards are built around a **Nordic nRF52840** SoC (ARM Cortex-M4F, BLE 5, 802.15.4/Thread radio) in an **Adafruit Feather-compatible** form factor, and were sold as part of Particle's "Gen 3" / **Particle Mesh** hardware line, launched in 2018.

| | **Argon** | **Xenon** |
|---|---|---|
| Radios | nRF52840 (BLE/Thread) + **ESP32 co-processor (Wi-Fi)** | nRF52840 (BLE/Thread) only — **no Wi-Fi, no cellular** |
| Original role | Mesh "gateway" — bridges a local mesh network to the internet | Mesh "endpoint/repeater" — talks only to a nearby gateway |
| Particle lifecycle status | **Deprecated** — official end of support **March 31, 2025** | **End of Life** — support ended **December 31, 2020** |
| Product page | [docs.particle.io/argon](https://docs.particle.io/argon/) | [docs.particle.io/xenon](https://docs.particle.io/xenon/) |

They were originally sold together (with a cellular sibling, **Boron**) as pieces of a three-tier mesh architecture: cheap, battery-friendly Xenons as sensor nodes, with a single Argon or Boron per cluster acting as the internet-facing gateway over Thread (802.15.4). Particle killed the Thread/OpenThread mesh feature entirely in January 2020, and by the end of that year had removed the ability to create any new mesh networks — so today, a Xenon has no supported way to reach the internet at all (it never had its own internet radio, and its only path there — mesh — no longer works). An Argon can still reach the internet directly since it's Wi-Fi native, though on a best-effort basis given its own support has also ended.

### 1.2 What firmware is currently on them

Out of the box (and unless you've already overwritten it), these boards run **two layered pieces of Particle software**:

1. **Bootloader** — the low-level code that decides whether to boot into the installed application or drop into DFU/firmware-update mode.
2. **Device OS** — Particle's proprietary system firmware. This is what manages the Wi-Fi/BLE setup handshake, the status LED, the connection to Particle's cloud, and OTA updates. It sits between the hardware and whatever "user application" is installed.
3. **User application** — the actual app code running on top of Device OS. Unless you (or a previous owner) has flashed something custom, this is **Tinker**, Particle's stock demo firmware.

**Tinker** exposes the board's GPIO pins to Particle's cloud/mobile app, letting you call familiar Wiring-style functions (`digitalWrite`, `digitalRead`, `analogWrite`, `analogRead`) remotely without writing any code:
- Open the pin list in the [Particle mobile app](https://www.particle.io/) or the [Particle Console](https://console.particle.io/), tap a pin, choose a function (e.g., "Digital Write"), and trigger it.
- If Tinker isn't currently installed, the app/Console has a "Flash Tinker" option — note this **overwrites whatever is currently on the board**.
- Docs: [Tinker & Mobile App — Argon](https://docs.particle.io/tutorials/developer-tools/tinker/argon/) · [Tinker & Mobile App — Xenon](https://docs.particle.io/tutorials/developer-tools/tinker/xenon/)

**Important limitation**: Tinker's remote control only works once the board is (a) connected to Wi-Fi/mesh and (b) claimed to a Particle account, since it operates through Particle's cloud API. For an Argon this is achievable (see §1.3). For a Xenon, since there's no functioning mesh gateway path left, Tinker cannot be triggered remotely today — the board is otherwise inert until you flash your own firmware.

### 1.3 Using the stock firmware — pairing and setup

To get Device OS + Tinker actually connected and controllable:

1. Put the board in **Listening Mode**: hold the **MODE** button for 3 seconds until the LED blinks **dark blue**.
2. Provision Wi-Fi credentials (Argon) via one of:
   - **Particle mobile app** (recommended) — scans for the board over **Bluetooth LE**, not Wi-Fi. Argon/Xenon do **not** broadcast their own Wi-Fi hotspot/SoftAP the way older Particle boards (e.g. Photon) did — SoftAP setup was removed on Gen 3 hardware. All setup goes over BLE or USB serial.
   - **USB serial**, via the CLI: `particle usb list` to confirm it enumerates, then `particle serial wifi`.
3. Watch the LED cycle: blinking green (joining Wi-Fi) → blinking cyan (joining the Particle cloud) → **breathing cyan** (connected and running normally).
4. Claim the device to your account in the mobile app or Console so it shows up there and Tinker becomes remotely controllable.

Docs: [Wi-Fi setup options](https://docs.particle.io/reference/device-os/wifi-setup-options/) · [How to set up Argon/Boron via USB](https://docs.particle.io/troubleshooting/guides/device-management/how-can-i-set-up-my-argon-or-boron-via-usb/) · [Argon quickstart](https://docs.particle.io/quickstart/argon/)

### 1.4 What it shows up as (USB identification)

Particle's official USB vendor ID is `0x2B04`. Each board/mode combination has its own product ID, useful for confirming what state a board is in from your computer's USB device list (`system_profiler SPUSBDataType` on macOS, `lsusb` on Linux, Device Manager on Windows):

| Board | Mode | USB Vendor:Product ID | Shows up as |
|---|---|---|---|
| Argon | Normal running (CDC/serial) | `2B04:C00C` | `Particle Argon` |
| Argon | DFU (bootloader) | `2B04:D00C` | `Argon DFU Mode` |
| Xenon | Normal running (CDC/serial) | `2B04:C00E` | `Particle Xenon` |
| Xenon | DFU (bootloader) | `2B04:D00E` | `Xenon DFU Mode` |
| Boron *(for reference)* | Normal / DFU | `2B04:C00D` / `2B04:D00D` | `Particle Boron` / `Boron DFU Mode` |

`particle usb list` (Particle CLI) will also report the device's unique 24-character **Device ID** once it enumerates — this is the board's permanent hardware identifier, independent of Particle cloud login.

### 1.5 Device modes and status LED reference

| LED pattern | Meaning |
|---|---|
| **Breathing cyan** (slow pulse) | Connected to Wi-Fi *and* the Particle cloud, running normally |
| **Blinking green** | Connecting to the Wi-Fi network |
| **Blinking cyan** (fast) | Wi-Fi joined, now connecting to the Particle cloud |
| **Blinking dark blue** | **Listening Mode** — no Wi-Fi credentials set, waiting to be configured via BLE or USB serial |
| **Blinking yellow** | **DFU mode** — bootloader active, waiting for new firmware over USB |
| **Blinking magenta** | Firmware actively being written — do not disconnect |
| 3 orange blinks | Reached the internet but couldn't reach Particle's cloud (bad server keys) |
| 1 red blink | Generic cloud handshake error |
| Solid/blinking red | Hardware or firmware fault |

To force **DFU mode**: hold **MODE**, tap **RESET** while still holding MODE, watch the LED go magenta then yellow, release once it's blinking yellow. Docs: [Status LED and Device Modes — Argon](https://docs.particle.io/tutorials/device-os/led/argon/) · [Status LED and Device Modes — Xenon](https://docs.particle.io/tutorials/device-os/led/xenon/)

### 1.6 Compatibility — what works "off the shelf"

| Tool | Works out of the box on stock firmware? | Notes |
|---|---|---|
| **Particle CLI / Workbench** | ✅ Yes | The native, official toolchain. `particle flash --usb`, `particle compile`, etc. work immediately over USB DFU with no hardware changes. |
| **Particle mobile app / Console** | ✅ Yes | Native pairing and Tinker control, as above. |
| **Arduino IDE** | ❌ Not natively | These boards **do not appear** in Arduino IDE's default board list, and Particle's stock bootloader doesn't speak the protocol the Arduino IDE expects (auto-reset + STK500/UF2-style upload). Support exists via Adafruit's community board package (`Adafruit_nRF52_Arduino` — add `https://adafruit.github.io/arduino-board-index/package_adafruit_index.json` as an Additional Board Manager URL, then install "Adafruit nRF52 by Adafruit"), **but** this generally requires first replacing Particle's bootloader with Adafruit's own (via an SWD programmer) — a one-way step covered in a later section. |
| **CircuitPython** | ❌ Not natively | Same story as Arduino — requires swapping in Adafruit's UF2 bootloader via SWD first. |
| **Zephyr RTOS** | ❌ Not natively | Flashes directly over SWD; effectively replaces Device OS and the bootloader entirely. |

In short: **as long as Particle's bootloader is still on the board, only Particle's own tools (CLI/Workbench/mobile app) work out of the box.** Everything else (Arduino, CircuitPython, Zephyr) requires an SWD programmer and a firmware/bootloader replacement step, which will be covered in a later section of this document.

### 1.7 Official reference links

- [Particle Argon — product/docs home](https://docs.particle.io/argon/)
- [Particle Xenon — product/docs home](https://docs.particle.io/xenon/)
- [Argon quickstart guide](https://docs.particle.io/quickstart/argon/)
- [Wi-Fi setup options (Device OS reference)](https://docs.particle.io/reference/device-os/wifi-setup-options/)
- [Tinker & Mobile App — Argon](https://docs.particle.io/tutorials/developer-tools/tinker/argon/) / [Xenon](https://docs.particle.io/tutorials/developer-tools/tinker/xenon/)
- [Status LED and Device Modes — Argon](https://docs.particle.io/tutorials/device-os/led/argon/) / [Xenon](https://docs.particle.io/tutorials/device-os/led/xenon/)
- [Particle CLI reference](https://docs.particle.io/reference/developer-tools/cli/)
- [Particle Workbench setup](https://docs.particle.io/getting-started/developer-tools/workbench/)
- [Mesh deprecation announcement (why Xenon is cloud-orphaned)](https://www.particle.io/blog/mesh-deprecation/)
- [Product Lifecycle Policy and Status (official EOL dates)](https://docs.particle.io/reference/product-lifecycle/product-lifecycle-policy-status/)
- [Adafruit_nRF52_Arduino (community Arduino board support)](https://github.com/adafruit/Adafruit_nRF52_Arduino)

---

## 2. Prerequisites for Either Custom Firmware Path

Both paths below **replace Particle's bootloader** — this is a one-way step for as long as the new bootloader is installed (you can restore Particle's own bootloader later via the same method if you ever want the board back on Particle's stack). Both also need the same hardware and starting procedure:

- **An SWD programmer.** The official **Particle Debugger** works for this — its design is **CMSIS-DAP** (confirmed via Particle's own [design files](https://github.com/particle-iot/debugger)), *not* a Segger J-Link. This matters because most third-party guides (including Adafruit's own) assume `nrfjprog`, which only talks to J-Link hardware. With a CMSIS-DAP probe, use **OpenOCD** instead — it's a proven, documented alternative for exactly this chip. Particle Workbench installs a copy of OpenOCD that works with the Particle Debugger, so if Workbench is already installed you likely have it.
- **The 10-pin 2×5, 0.05" SWD ribbon cable** connecting the debugger to the board's dedicated debug header.
- Physical access to the header — shouldn't be blocked by a FeatherWing Tripler or similar stacking board, since the debug connector sits separately from the Feather header pins.

Generic OpenOCD invocation pattern used by both paths below (adjust config paths to whatever ships with your OpenOCD install):

```
openocd -f interface/cmsis-dap.cfg -f target/nrf52.cfg -c "init; reset halt; nrf5 mass_erase; exit"   # erase — irreversible, do this deliberately
openocd -f interface/cmsis-dap.cfg -f target/nrf52.cfg -c "program <file>.hex verify reset exit"        # write new bootloader/firmware
```

Reference: [Particle: Using SWD/JTAG](https://docs.particle.io/reference/developer-tools/jtag/) · [Particle Debugger datasheet](https://docs.particle.io/reference/datasheets/accessories/debugger/) · [OpenOCD + CMSIS-DAP + nRF52 walkthrough](https://gist.github.com/jeru/5a2e9e8df50ebe110754bb9d9a2a8846)

---

## 3. Version A — Arduino IDE / CircuitPython (Adafruit Bootloader Path)

**Start here if you want the beginner-friendly, familiar path.** This is the standard Arduino IDE workflow (or CircuitPython, if you'd rather edit Python files directly on the board) — the largest ecosystem of tutorials, libraries, and examples of any option here. Both sub-paths **depend on first installing Adafruit's own nRF52 bootloader** in place of Particle's; they diverge only after that shared step, and you can reflash between them later since the bootloader stays the same for both.

### 3.1 Shared step: install Adafruit's bootloader
1. Get the correct `.hex` bootloader build for your board from [Adafruit's nRF52 Bootloader releases](https://github.com/adafruit/Adafruit_nRF52_Bootloader) (PCA10056-based build family covers the nRF52840).
2. Using the OpenOCD pattern from §2: mass-erase, then `program adafruit_bootloader.hex verify reset exit`.
3. On success, the board **re-enumerates as a USB mass-storage drive** (e.g. `ARGONBOOT`/`XENONBOOT`) — this is Adafruit's bootloader confirming it's alive, and is now your entry point for all future updates (no SWD probe needed again unless something goes wrong).
4. Double-tapping **RESET** within 500ms re-enters this bootloader drive at any time going forward.

### 3.2a If going Arduino
1. In Arduino IDE → Preferences → Additional Board Manager URLs, add:
   `https://adafruit.github.io/arduino-board-index/package_adafruit_index.json`
2. Tools → Board → Boards Manager → install **"Adafruit nRF52 by Adafruit."**
3. Select the matching board from Tools → Board (Xenon has explicit support in this package; Argon can be selected as a compatible nRF52840 Feather variant).
4. Write a normal sketch (`setup()`/`loop()`, standard Arduino API) and Upload as usual — the Adafruit bootloader handles the auto-reset/upload handshake the Arduino IDE expects.

Note: this is a **separate project from Zephyr's `ArduinoCore-zephyr`** bridge (which lets some newer official Arduino boards run Zephyr underneath while still using Arduino-style sketches). `Adafruit_nRF52_Arduino` is not Zephyr-based — it's built on Nordic's nRF5 SDK. If your goal is specifically "run Zephyr," use Version B instead; this path gives you Arduino's API but not Zephyr's OS.

Reference: [Adafruit_nRF52_Arduino](https://github.com/adafruit/Adafruit_nRF52_Arduino) · [Adafruit Bluefruit nRF52 Feather Arduino BSP setup guide](https://learn.adafruit.com/bluefruit-nrf52-feather-learning-guide/arduino-bsp-setup)

### 3.2b If going CircuitPython instead
1. Download the matching CircuitPython `.uf2` build for Argon or Xenon.
2. Drag and drop it onto the `ARGONBOOT`/`XENONBOOT` drive from §3.1.
3. Board reboots into CircuitPython; edit `code.py` directly on the `CIRCUITPY` drive that appears from then on — no separate compiler/IDE required for basic use.

Reference: [Adafruit: Build & Flash Particle (CircuitPython on the nRF52)](https://learn.adafruit.com/circuitpython-on-the-nrf52/build-flash-particle)

---

## 4. Version B — Zephyr

Zephyr flashes **directly** over SWD with no separate bootloader-swap step — the image you build already contains everything needed to boot. This is the more "from scratch," ground-up option: you write plain C against Zephyr's own APIs, not Arduino-style code, in exchange for a full real-time OS and real networking stacks.

### 4.1 What you get
- A real RTOS: kernel, threads, drivers, and networking stacks (BLE, Bluetooth Mesh, Thread/OpenThread, IP) — see the intro to Zephyr covered earlier in this project's chat history for the full architectural picture.
- Full control of the nRF52840 radio directly, no Particle abstraction layer in between.
- **Argon Wi-Fi caveat**: the ESP32 co-processor is wired to the nRF52840 over **UART only** (SPI pins aren't connected on this board), so Wi-Fi requires Zephyr's `espressif,esp-at` driver talking AT commands over serial — not a native high-throughput SPI Wi-Fi driver. Enable with `CONFIG_NETWORKING=y`, `CONFIG_WIFI=y`, `CONFIG_WIFI_ESP_AT=y`.
- Both boards have **upstream-maintained board targets** in mainline Zephyr: `particle_argon` and `particle_xenon` — genuinely first-class support, not a community patch.

### 4.2 Setup
1. Install the Zephyr SDK and `west` (Zephyr's meta build tool) per the [official getting-started guide](https://docs.zephyrproject.org/latest/develop/getting_started/index.html).
2. Initialize a workspace and pull the Zephyr source tree (`west init`, `west update`).
3. Build a sample against the correct board target:
   ```
   west build -b particle_argon samples/basic/blinky
   ```
   (swap `particle_xenon` for the Xenon)
4. Flash over SWD using the OpenOCD pattern from §2, or `west flash` if your `west` config is already pointed at OpenOCD/CMSIS-DAP.

### 4.3 Networking architecture options
Since Zephyr doesn't impose Particle's old mesh topology, you choose:
- **Recreate the original shape**: Xenons as **Thread** mesh nodes (Zephyr bundles OpenThread), Argon as a **Thread Border Router** bridging to Wi-Fi. Real integration work — Particle built and maintained that bridging logic themselves; on Zephyr you're assembling it from OpenThread primitives. Note: some concurrent BLE+OpenThread radio-sharing issues have been reported on nRF52840 in Zephyr's tracker — worth checking current status before committing to this design.
- **Bluetooth Mesh** instead of Thread — natively supported, arguably a simpler model (any GATT-capable node can proxy to a phone, no dedicated border router role).
- **No mesh at all** — each board runs fully independently as its own BLE (and, for Argon, Wi-Fi) device.

Reference: [Zephyr — Particle Argon board docs](https://docs.zephyrproject.org/latest/boards/particle/argon/doc/index.html) · [Zephyr — Particle Xenon board docs](https://docs.zephyrproject.org/latest/boards/particle/xenon/doc/index.html) · [OpenThread on Zephyr](https://openthread.io/platforms/rtos/zephyr) · [Golioth: ESP32 AT + Zephyr Wi-Fi](https://blog.golioth.io/use-the-esp32-at-binary-to-make-any-zephyr-project-wi-fi-enabled/)

---

## 5. Choosing Between the Two Versions

| | **Arduino / CircuitPython (§3)** | **Zephyr (§4)** |
|---|---|---|
| Language | C++ (Arduino) or Python (CircuitPython) | C, Zephyr-native APIs |
| Bootloader swap needed? | Yes — Adafruit bootloader, one-time SWD step | No — flashes directly |
| Networking depth | BLE via Arduino/CircuitPython libraries; no built-in mesh stack | Full: raw BLE, Thread/OpenThread mesh, Bluetooth Mesh, IP stack |
| Best fit | Fastest path to "something running," most beginner-friendly, huge existing library ecosystem | Rebuilding a real mesh network, or wanting an industrial-grade RTOS under your own code |
| Maturity for these exact boards | Mature (Adafruit maintains real Xenon/Argon support) but a second-hand fit — not built with these boards specifically in mind | Genuinely first-class (`particle_argon`/`particle_xenon` upstream targets) |

*Further sections (concrete sample projects for each version) to follow.*
