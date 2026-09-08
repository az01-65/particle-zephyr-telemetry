# Getting Particle Argon & Xenon Working (Deprecated / EOL Boards)

A practical guide for anyone who has picked up a Particle **Argon** or **Xenon** board today, now that Particle has wound down support for both. Covers identifying board state, pairing/connecting, flashing custom firmware, and — critically — what's actually still possible on each board given their EOL status.

---

## 1. Background: what you actually have

Both boards are part of Particle's "Gen 3" / **Particle Mesh** line (launched 2018), built around a Nordic **nRF52840** SoC in an Adafruit Feather form factor:

| | Argon | Xenon |
|---|---|---|
| Radios | nRF52840 (BLE) + **ESP32 co-processor (Wi-Fi)** | nRF52840 (BLE) only — **no Wi-Fi, no cellular** |
| Original role | Mesh "gateway" — bridges local mesh to internet | Mesh "endpoint/repeater" — talks only to a gateway |
| Particle lifecycle status | **Deprecated**, end-of-support **March 31, 2025** | **End of Life** since **December 31, 2020** |

**Why this matters:** Particle killed the entire Thread/OpenThread mesh feature in January 2020 — it was judged too complex to make reliable, and the wrong technology choice in hindsight. By Dec 31, 2020, the mobile app and cloud APIs stopped allowing *any* new mesh networks or new devices added to existing ones.

**The critical asymmetry:**
- **Xenon has no radio capable of reaching the internet on its own.** Its only path to the Particle cloud was always *through* a mesh network to a gateway (Argon/Boron). With mesh network creation permanently disabled, a Xenon picked up today is effectively **cloud-orphaned** — there is no supported way to get it talking to Particle's servers anymore, full stop.
- **Argon can still reach the cloud directly** (it's Wi-Fi native, no mesh dependency) — but its own official support ended March 2025, so treat any cloud/dashboard interaction as **best-effort, not guaranteed**.

**Practical conclusion:** for Xenon, skip straight to **Path B** below. For Argon, Path A is worth trying first since it's low-effort, but don't be surprised if Particle's cloud pathway is flaky or fully dead by the time you read this.

---

## 2. Status LED reference

Before doing anything, read the RGB LED to know what state the board is in:

| LED pattern | Meaning |
|---|---|
| **Breathing cyan** (slow pulse) | Connected to Wi-Fi *and* the Particle cloud, running normally |
| **Blinking green** | Connecting to the Wi-Fi network |
| **Blinking cyan** (fast) | Wi-Fi joined, now connecting to the Particle cloud |
| **Blinking dark blue** | **Listening Mode** — no Wi-Fi credentials set, waiting to be configured via BLE or USB serial |
| **Blinking yellow** | **DFU mode** — bootloader is active, waiting for new firmware over USB |
| **Blinking magenta** | Firmware actively being flashed — do not disconnect |
| 3 orange blinks | Reached the internet but couldn't reach Particle's cloud (bad server keys) |
| 1 red blink | Generic cloud handshake error |
| Solid/blinking red | Hardware or firmware fault |

---

## 3. Path A — Stay on Particle's stack (Argon only, best-effort)

Goal: get the Argon onto your Wi-Fi and running Particle's Device OS + your own app code.

**You need:** a phone with the Particle mobile app, OR a computer with a genuinely data-capable USB cable (many phone-charging cables are power-only and won't work) and `particle-cli` installed.

### 3.1 Pair over Bluetooth (recommended)
1. Install the **Particle mobile app** (iOS/Android) and sign in / create an account.
2. If the board is showing blinking dark blue (Listening Mode), the app will find it via a BLE scan — this doesn't depend on your computer's USB or Wi-Fi at all, just Bluetooth range on your phone.
3. Add device → select the Argon → enter your Wi-Fi SSID/password → app pushes credentials over BLE.
4. Watch the LED: blinking green (joining Wi-Fi) → blinking cyan (joining cloud) → breathing cyan (success).

If it's not in Listening Mode, force it: hold the **MODE** button for 3 seconds until the LED starts blinking dark blue.

### 3.2 Or set up over USB
```
particle usb list        # confirm the board enumerates
particle serial wifi     # push Wi-Fi credentials over serial instead of BLE
```

### 3.3 Write and flash your own application
```
particle compile argon myapp/        # or use Particle Workbench (VS Code)
```
Then put the board in DFU mode (see §4) and:
```
particle flash --usb myapp.bin
```

---

## 4. DFU mode — what it is, and how to use it

**DFU (Device Firmware Upgrade)** is a USB standard letting the chip's bootloader receive new firmware directly, independent of whatever application firmware is (or isn't) currently installed. It's the recovery/flashing layer underneath everything else — useful even if the board is bricked or stuck, since the bootloader is separate from your app code.

**To enter DFU mode:**
1. Hold **MODE**.
2. Tap **RESET** while still holding MODE.
3. Watch the LED: it blinks magenta, then yellow.
4. Release MODE once it's blinking **yellow**.

**To flash while in DFU mode:**
```
particle flash --usb <file>.bin
```

DFU mode is *not* the same as Listening Mode — Listening Mode is your existing, working firmware asking for Wi-Fi info; DFU mode is the bootloader itself, waiting to overwrite firmware entirely. Use DFU when you need to install new firmware or recover a broken board — not for routine Wi-Fi setup.

---

## 5. Path B — Bare nRF52840, independent of Particle (Argon *and* Xenon)

Goal: treat the board as generic Adafruit-Feather-footprint nRF52840 hardware and run something other than Particle's stack. **This is the only realistic path for Xenon**, and a solid option for Argon too, since it removes any dependency on Particle's cloud or account system.

**You need:** an SWD programmer (a Segger J-Link, or one of the cheap J-Link-compatible clone boards) and jumper wires to the board's **10-pin SWD debug header**.

1. Wire the SWD probe to the debug header.
2. Depending on target firmware:
   - **CircuitPython / Arduino**: use Nordic's `nrfjprog` to erase flash and burn Adafruit's nRF52 bootloader. The board then re-enumerates as a mass-storage drive (e.g. `ARGONBOOT`/`XENONBOOT`) — drag-and-drop a `.uf2` build to install CircuitPython, or install the `Adafruit_nRF52_Arduino` board package and upload sketches normally via the Arduino IDE from then on.
   - **Zephyr RTOS**: both boards have upstream-maintained board targets (`particle_argon`, `particle_xenon`) in mainline Zephyr — no bootloader swap needed, flash straight over SWD with `west flash`.
   - **Bare Nordic nRF5 SDK**: fully viable too — it's the same chip Nordic sells standalone dev kits for.
3. From this point on you have full control of the hardware with zero dependency on Particle's cloud, mobile app, or account.

**Note:** this permanently replaces Particle's bootloader. The board stops being usable with `particle flash` or Particle's cloud unless you later reflash Particle's own bootloader back via SWD.

---

## 6. Troubleshooting

- **USB not enumerating at all**: try a different cable — many are charge-only with no data lines. Confirm with `particle usb list` or `system_profiler SPUSBDataType` (macOS) / `lsusb` (Linux). Also check you're not running in a sandboxed/virtualized shell without real USB passthrough to the host.
- **Stuck in a boot loop or unresponsive**: enter DFU mode (§4) and reflash Device OS, or fall back to the SWD recovery path (§5) if DFU itself won't respond.
- **Argon won't reach the Particle cloud even after Wi-Fi connects**: expected, given EOL status — treat as unsupported. Consider Path B.
- **Xenon shows any LED activity at all but never reaches "breathing cyan"**: expected and not fixable within the Particle ecosystem — there's no supported way for it to reach the cloud anymore. Move to Path B.

---

## 7. Reference

- [Particle: Status LED and Device Modes — Argon](https://docs.particle.io/tutorials/device-os/led/argon/)
- [Particle: Device Blinking Dark Blue (Listening Mode)](https://docs.particle.io/troubleshooting/guides/device-troubleshooting/device-blinking-dark-blue/)
- [Particle: How to set up Argon/Boron via USB](https://docs.particle.io/troubleshooting/guides/device-management/how-can-i-set-up-my-argon-or-boron-via-usb/)
- [Particle: Wi-Fi setup options](https://docs.particle.io/reference/device-os/wifi-setup-options/)
- [Particle blog: Mesh deprecation announcement](https://www.particle.io/blog/mesh-deprecation/)
- [Particle: Mesh deprecation reference/timeline](https://docs.particle.io/reference/discontinued/mesh/)
- [Particle: Product Lifecycle Policy and Status](https://docs.particle.io/reference/product-lifecycle/product-lifecycle-policy-status/)
- [Adafruit: Build & Flash Particle (CircuitPython path)](https://learn.adafruit.com/circuitpython-on-the-nrf52/build-flash-particle)
- [Zephyr: Particle Argon board docs](https://docs.zephyrproject.org/latest/boards/particle/argon/doc/index.html)
- [GitHub: particle-iot/argon hardware design files](https://github.com/particle-iot/argon)
