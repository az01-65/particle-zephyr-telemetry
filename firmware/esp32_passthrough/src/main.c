/*
 * esp32_passthrough - transparent USB<->UART bridge to the Particle Argon's
 * onboard ESP32 Wi-Fi co-processor, plus ESP32 boot/reset control.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * -----------------------------------------------------------------------
 * Why this exists
 * -----------------------------------------------------------------------
 * The Argon's ESP32 has no USB connection of its own - it only talks to the
 * nRF52840 over a UART (see boards/particle_argon.overlay). Stock Particle
 * Device OS uses that link to run Espressif's AT-command NCP firmware; there
 * is no way to flash arbitrary Arduino/esptool.py code onto the ESP32
 * without something bridging the Argon's real USB port through to that
 * UART, because esptool.py/Arduino IDE need to talk to a serial port
 * directly, not to Particle's own AT-command protocol.
 *
 * This app IS that bridge. Once flashed, the Argon's existing USB port
 * behaves exactly like a normal, directly-USB-attached ESP32 dev board:
 * point Arduino IDE or esptool.py at it and upload like any other ESP32
 * board, no manual button presses.
 *
 * -----------------------------------------------------------------------
 * How it actually works
 * -----------------------------------------------------------------------
 * Section map:
 *   1. USB device bring-up        - manual USBD setup (needs a message
 *                                    callback the simpler auto-init path
 *                                    this project's other apps use can't
 *                                    provide)
 *   2. ESP32 boot-mode/reset control - the DTR/RTS -> ESPBOOT/ESPEN mapping
 *   3. USBD message callback      - wires sections 1 and 2 together, plus
 *                                    baud-rate sync for the bridge below
 *
 * Two separate mechanisms make the actual bridge work, both necessary:
 *
 * A. Raw byte bridging + baud-rate sync between the USB-CDC port and the
 *    ESP32's UART (uart1). Handled entirely by Zephyr's own in-tree
 *    zephyr,uart-bridge devicetree node (see boards/particle_argon.overlay)
 *    and drivers/serial/uart_bridge.c - this app only has to call
 *    uart_bridge_settings_update() when the host changes its line coding
 *    (baud rate, section 3), which is standard usage of that driver's own
 *    public API, modeled on zephyr's own
 *    samples/subsys/usb/cdc_acm_bridge sample.
 *
 * B. ESP32 boot-mode/reset control (section 2) - NOT handled by the generic
 *    bridge above, since that only knows about data and line coding. A standard
 *    ESP32 dev board's onboard USB-serial chip drives the ESP32's EN
 *    (reset) and GPIO0 (boot-mode select) pins from the host's DTR/RTS
 *    control lines, which is what lets esptool.py/Arduino IDE reset the
 *    chip into its UART bootloader automatically before every upload,
 *    with no physical button press. This app reproduces that exact
 *    behavior digitally: it watches the host's DTR/RTS state (via
 *    uart_line_ctrl_get() on the CDC-ACM device) and drives the Argon's
 *    ESPBOOT/ESPEN GPIOs (see boards/particle_argon.overlay for the pin
 *    numbers and where they come from) using the same DTR/RTS -> EN/GPIO0
 *    mapping esptool.py's classic_reset() strategy assumes:
 *
 *      DTR asserted   -> ESPBOOT selects download/bootloader mode
 *      DTR deasserted -> ESPBOOT selects normal firmware boot
 *      RTS asserted   -> ESPEN holds the ESP32 in reset
 *      RTS deasserted -> ESPEN releases the ESP32 to run
 *
 * This is a standalone firmware image, not a mode of argon_gateway - see
 * prj.conf for why flashing this replaces whatever else is on the Argon's
 * nRF52840, and does not persist any state or interact with the mesh
 * project in any way.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/uart/uart_bridge.h>
#include <zephyr/usb/usbd.h>

#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(esp32_passthrough, LOG_LEVEL_INF);

/* ===========================================================================
 * 1. USB device bring-up
 * ===========================================================================
 * Manual USBD setup rather than this project's other two apps' simpler
 * CONFIG_CDC_ACM_SERIAL_INITIALIZE_AT_BOOT=y auto-init, because this app
 * needs usbd_msg_register_cb() (section 2) - the auto-init path has no hook
 * for it. Modeled on zephyr/samples/subsys/usb/cdc_acm_bridge's own manual
 * bring-up, but deliberately NOT reusing that sample's own
 * samples/subsys/usb/common helper: that helper's own Kconfig fragment
 * states outright "you cannot use them in your own application" - it is
 * sample-scaffolding, not a supported library. Everything below instead
 * calls the same real, public USBD APIs that helper itself calls, trimmed
 * to what this single-CDC-ACM, full-speed-only (nRF52840 has no USB
 * high-speed PHY) app actually needs.
 *
 * VID 0x2fe3 is the same placeholder "Zephyr Project" vendor ID this
 * project's other two apps already use by way of their own auto-init
 * default (visible over USB as Vendor ID 0x2fe3 on both) - not a real
 * assigned VID, fine for a hobby/demo project, not for a real product.
 */
#define ESP32_PASSTHROUGH_VID 0x2fe3
#define ESP32_PASSTHROUGH_PID 0x0005

USBD_DEVICE_DEFINE(esp32_usbd, DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0)), ESP32_PASSTHROUGH_VID,
		    ESP32_PASSTHROUGH_PID);

USBD_DESC_LANG_DEFINE(esp32_usbd_lang);
USBD_DESC_MANUFACTURER_DEFINE(esp32_usbd_mfr, "Zephyr Project");
USBD_DESC_PRODUCT_DEFINE(esp32_usbd_product, "ESP32 Passthrough");

USBD_DESC_CONFIG_DEFINE(esp32_usbd_fs_cfg_desc, "FS Configuration");
USBD_CONFIGURATION_DEFINE(esp32_usbd_fs_config, USB_SCD_SELF_POWERED, 125, &esp32_usbd_fs_cfg_desc);

/* Forward declaration - defined in section 2, registered in section 3. */
static void esp32_usbd_msg_cb(struct usbd_context *const ctx, const struct usbd_msg *msg);

static int esp32_usbd_bringup(void)
{
	int err;

	err = usbd_add_descriptor(&esp32_usbd, &esp32_usbd_lang);
	if (err) {
		LOG_ERR("Failed to add language descriptor (err %d)", err);
		return err;
	}

	err = usbd_add_descriptor(&esp32_usbd, &esp32_usbd_mfr);
	if (err) {
		LOG_ERR("Failed to add manufacturer descriptor (err %d)", err);
		return err;
	}

	err = usbd_add_descriptor(&esp32_usbd, &esp32_usbd_product);
	if (err) {
		LOG_ERR("Failed to add product descriptor (err %d)", err);
		return err;
	}

	err = usbd_add_configuration(&esp32_usbd, USBD_SPEED_FS, &esp32_usbd_fs_config);
	if (err) {
		LOG_ERR("Failed to add Full-Speed configuration (err %d)", err);
		return err;
	}

	err = usbd_register_all_classes(&esp32_usbd, USBD_SPEED_FS, 1, NULL);
	if (err) {
		LOG_ERR("Failed to register USB classes (err %d)", err);
		return err;
	}

	/* Use class code information from the CDC-ACM interface descriptors
	 * rather than a device-level class code - standard for a
	 * single-function CDC-ACM device, matches how this project's other
	 * two apps' auto-init path (and the upstream sample this is modeled
	 * on) both configure it.
	 */
	usbd_device_set_code_triple(&esp32_usbd, USBD_SPEED_FS, 0, 0, 0);
	usbd_self_powered(&esp32_usbd, true);

	err = usbd_msg_register_cb(&esp32_usbd, esp32_usbd_msg_cb);
	if (err) {
		LOG_ERR("Failed to register USBD message callback (err %d)", err);
		return err;
	}

	err = usbd_init(&esp32_usbd);
	if (err) {
		LOG_ERR("Failed to initialize USB device support (err %d)", err);
		return err;
	}

	return 0;
}

/* ===========================================================================
 * 2. ESP32 boot-mode/reset control
 * ===========================================================================
 * See this file's header comment for the DTR/RTS -> ESPBOOT/ESPEN mapping
 * this reproduces, and boards/particle_argon.overlay for where the pin
 * numbers come from and why they can be trusted despite not being
 * documented anywhere in Zephyr itself.
 */

static const struct gpio_dt_spec espboot = GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), espboot_gpios);
static const struct gpio_dt_spec espen = GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), espen_gpios);

/* Named for readability at each call site below - these are the *logical*
 * (gpio_pin_set_dt) values, already accounting for the ACTIVE_HIGH/
 * ACTIVE_LOW polarity each pin is declared with in the overlay, not raw
 * electrical levels.
 */
#define ESPBOOT_NORMAL 1   /* electrically HIGH: boot normal firmware */
#define ESPBOOT_DOWNLOAD 0 /* electrically LOW: select UART bootloader */
#define ESPEN_RELEASE 0    /* open-drain released (pulled up by the module): running */
#define ESPEN_ASSERT 1     /* open-drain driven low: held in reset */

static int esp32_control_gpios_init(void)
{
	int err;

	if (!gpio_is_ready_dt(&espboot)) {
		LOG_ERR("ESPBOOT GPIO not ready");
		return -ENODEV;
	}
	if (!gpio_is_ready_dt(&espen)) {
		LOG_ERR("ESPEN GPIO not ready");
		return -ENODEV;
	}

	/* Default state before any host activity: normal boot, running -
	 * exactly how the ESP32 should sit if nothing ever opens the serial
	 * port at all.
	 */
	err = gpio_pin_configure_dt(&espboot, GPIO_OUTPUT);
	if (err) {
		LOG_ERR("Failed to configure ESPBOOT (err %d)", err);
		return err;
	}
	err = gpio_pin_set_dt(&espboot, ESPBOOT_NORMAL);
	if (err) {
		return err;
	}

	err = gpio_pin_configure_dt(&espen, GPIO_OUTPUT);
	if (err) {
		LOG_ERR("Failed to configure ESPEN (err %d)", err);
		return err;
	}
	err = gpio_pin_set_dt(&espen, ESPEN_RELEASE);
	if (err) {
		return err;
	}

	LOG_INF("ESP32 control GPIOs ready (normal boot, running)");
	return 0;
}

/* Tracks the last-applied state of one control line, purely so transitions
 * get one clean log line each, instead of one every time the host happens
 * to re-send the same control-line-state value. "known" is false until the
 * first real reading comes in, so that reading is always applied even if
 * it happens to be false/deasserted (which would otherwise look identical
 * to the zero-initialized default and be skipped).
 */
struct line_signal_state {
	bool known;
	bool value;
};

static struct line_signal_state last_dtr;
static struct line_signal_state last_rts;

static void esp32_sync_reset_state(const struct device *cdc_dev)
{
	uint32_t dtr_raw = 0;
	uint32_t rts_raw = 0;
	int err;

	err = uart_line_ctrl_get(cdc_dev, UART_LINE_CTRL_DTR, &dtr_raw);
	if (err) {
		LOG_WRN("Failed to read DTR (err %d)", err);
		return;
	}
	err = uart_line_ctrl_get(cdc_dev, UART_LINE_CTRL_RTS, &rts_raw);
	if (err) {
		LOG_WRN("Failed to read RTS (err %d)", err);
		return;
	}

	bool dtr = (dtr_raw != 0);
	bool rts = (rts_raw != 0);

	if (!last_dtr.known || dtr != last_dtr.value) {
		gpio_pin_set_dt(&espboot, dtr ? ESPBOOT_DOWNLOAD : ESPBOOT_NORMAL);
		LOG_INF("DTR %s -> ESPBOOT %s", dtr ? "asserted" : "deasserted",
			dtr ? "download" : "normal");
		last_dtr = (struct line_signal_state){.known = true, .value = dtr};
	}

	if (!last_rts.known || rts != last_rts.value) {
		gpio_pin_set_dt(&espen, rts ? ESPEN_ASSERT : ESPEN_RELEASE);
		LOG_INF("RTS %s -> ESPEN %s", rts ? "asserted" : "deasserted",
			rts ? "reset" : "released");
		last_rts = (struct line_signal_state){.known = true, .value = rts};
	}
}

/* ===========================================================================
 * 3. USBD message callback
 * ===========================================================================
 * Runs in the USB stack's own thread context (confirmed by the upstream
 * cdc_acm_bridge sample this is modeled on, which itself makes a blocking
 * uart_configure() call directly from the same callback) - a normal thread,
 * not an ISR, so both of the calls below are safe here.
 */
static void esp32_usbd_msg_cb(struct usbd_context *const ctx, const struct usbd_msg *msg)
{
	ARG_UNUSED(ctx);

	if (msg->type == USBD_MSG_CDC_ACM_LINE_CODING) {
		/* Propagate the host's chosen baud rate to uart1, via
		 * Zephyr's own uart_bridge driver - see this file's header
		 * comment, part 1.
		 */
		const struct device *bridge_dev = DEVICE_DT_GET(DT_NODELABEL(uart_bridge0));

		uart_bridge_settings_update(msg->dev, bridge_dev);
	}

	if (msg->type == USBD_MSG_CDC_ACM_CONTROL_LINE_STATE) {
		esp32_sync_reset_state(msg->dev);
	}
}

int main(void)
{
	int err;

	err = esp32_control_gpios_init();
	if (err) {
		return err;
	}

	err = esp32_usbd_bringup();
	if (err) {
		return err;
	}

	err = usbd_enable(&esp32_usbd);
	if (err) {
		LOG_ERR("Failed to enable USB device support (err %d)", err);
		return err;
	}

	LOG_INF("ESP32 passthrough ready");

	/* All real work happens in the USBD message callback (section 3) and
	 * the uart_bridge driver's own interrupt-driven pump - nothing left
	 * for this thread to do.
	 */
	k_sleep(K_FOREVER);

	return 0;
}
