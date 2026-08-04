/*
 * xenon_sensor - dual-mode telemetry node for the Particle Xenon (nRF52840)
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * -----------------------------------------------------------------------
 * Architecture, for the reviewer
 * -----------------------------------------------------------------------
 * This firmware boots into one of two mutually exclusive radio modes:
 *
 *   Mode 1 (BLE):    a GATT peripheral notifying telemetry to a connected
 *                     central. This is the original xenon_sensor behavior,
 *                     unchanged.
 *   Mode 2 (Thread):  an 802.15.4/Thread node that forms/joins a Thread
 *                     network using hardcoded credentials and periodically
 *                     sends the same telemetry over UDP multicast.
 *
 * Why a reboot is required to switch: the nRF52840 has a single 2.4GHz
 * radio. BLE and 802.15.4/Thread both want it, and Zephyr's BLE controller
 * and the 802.15.4 driver both want the same RADIO_IRQn vector - they
 * cannot usefully run at the same time on this hardware. So instead of
 * attempting to hot-swap radio stacks, the selected mode is persisted to
 * flash (NVS) and read once at boot; only that mode's stack is ever
 * started. Switching modes is a deliberate, explicit action (hold the
 * MODE button at boot) followed by a reboot - not a limitation to work
 * around, but the honest shape of the hardware constraint.
 *
 * Section map:
 *   1. Wire format                 - shared 8-byte telemetry struct
 *   2. Mode selection               - persisted mode, MODE button handling
 *   3. Shared sensor sampling       - die-temp + VDD read, used by both modes
 *   4. Mode 1: BLE                  - GATT service, advertising, connections
 *   5. Mode 2: Thread                - network join, UDP multicast send
 *   6. Sampling thread               - shared loop, dispatches by mode
 *   7. Entry point                   - mode load/switch, stack bring-up
 *
 * Concurrency/power notes carried over from the original BLE-only version
 * still apply: sensor sampling runs on its own K_THREAD_DEFINE() thread,
 * decoupled from main() and from the Bluetooth host / OpenThread work
 * queue threads, and spends nearly all its life blocked in k_sleep() so
 * Zephyr's tickless kernel can drop into deep idle between samples.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/util.h>
#include <zephyr/sys/reboot.h>

#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/dt-bindings/adc/nrf-saadc.h>

#include <zephyr/kvss/nvs.h>
#include <zephyr/storage/flash_map.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>

#include <zephyr/net/openthread.h>
#include <zephyr/net/socket.h>
#include <openthread/thread.h>
#include <openthread/instance.h>

#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(xenon_sensor, LOG_LEVEL_INF);

/* ===========================================================================
 * 1. Wire format - shared contract with argon_gateway (Mode 1) and with
 *    whatever eventually listens on the Thread multicast group (Mode 2)
 * ===========================================================================
 * Exactly 8 bytes, little-endian (native byte order on the Cortex-M4 in the
 * nRF52840, so no explicit byte-swapping is needed here). Do not reorder,
 * resize, or reinterpret these fields without updating consumers on both
 * sides.
 */
struct __packed telemetry_payload {
	int16_t temp_centi_c;	/* On-chip die temperature, in 0.01 degC steps (2350 = 23.50C) */
	uint16_t vdd_mv;	/* Regulated rail voltage as seen by the SAADC, in millivolts */
	uint32_t seq;		/* Monotonically incrementing sample counter */
};

BUILD_ASSERT(sizeof(struct telemetry_payload) == 8,
	     "telemetry_payload must stay exactly 8 bytes on the wire");

/* Sampling cadence. 5s keeps the demo responsive; a deployed sensor node
 * would likely stretch this to minutes to save even more power. Shared by
 * both modes.
 */
#define SAMPLE_PERIOD_S 5
#define SAMPLE_PERIOD K_SECONDS(SAMPLE_PERIOD_S)

/* ===========================================================================
 * 2. Mode selection
 * ===========================================================================
 * The persisted mode lives as a single uint8_t in the "storage_partition"
 * flash partition via the NVS key-value store. That partition already
 * exists upstream (nordic/nrf52840_partition.dtsi, pulled in by this
 * board's devicetree through mesh_feather.dtsi) - no devicetree overlay was
 * needed for this application.
 */

enum device_mode {
	MODE_BLE = 0,
	MODE_THREAD = 1,
};

#define MODE_NVS_ID 1U

static struct nvs_fs mode_fs;

/* Set once in main() before either mode's stack is brought up, and only
 * ever read afterward (by the sampling thread) - no lock needed.
 */
static enum device_mode current_mode;

/**
 * Mount the NVS file system used to persist the mode selection, sized off
 * the storage partition's real flash geometry rather than a guessed
 * constant.
 */
static int mode_storage_init(void)
{
	int err;
	struct flash_pages_info info;

	mode_fs.flash_device = PARTITION_DEVICE(storage_partition);
	if (!device_is_ready(mode_fs.flash_device)) {
		LOG_ERR("Storage partition flash device not ready");
		return -ENODEV;
	}

	mode_fs.offset = PARTITION_OFFSET(storage_partition);

	err = flash_get_page_info_by_offs(mode_fs.flash_device, mode_fs.offset, &info);
	if (err) {
		LOG_ERR("Unable to read flash page info for storage partition (err %d)", err);
		return err;
	}

	mode_fs.sector_size = info.size;
	/* Two sectors is the minimum NVS recommends for garbage collection to
	 * have somewhere to compact into; a single persisted byte will never
	 * come close to filling either one.
	 */
	mode_fs.sector_count = 2U;

	return nvs_mount(&mode_fs);
}

/**
 * Load the persisted mode. Defaults to (and persists) Mode 1/BLE the first
 * time this runs on a device, or if the stored value is missing/corrupt.
 */
static enum device_mode mode_load(void)
{
	uint8_t stored;
	ssize_t rc = nvs_read(&mode_fs, MODE_NVS_ID, &stored, sizeof(stored));

	if (rc == (ssize_t)sizeof(stored) && stored <= MODE_THREAD) {
		return (enum device_mode)stored;
	}

	LOG_INF("No valid persisted mode found; defaulting to Mode 1 (BLE)");
	stored = MODE_BLE;
	(void)nvs_write(&mode_fs, MODE_NVS_ID, &stored, sizeof(stored));

	return MODE_BLE;
}

/** Persist a mode selection. */
static int mode_store(enum device_mode mode)
{
	uint8_t value = (uint8_t)mode;
	ssize_t rc = nvs_write(&mode_fs, MODE_NVS_ID, &value, sizeof(value));

	return (rc < 0) ? (int)rc : 0;
}

static const char *mode_name(enum device_mode mode)
{
	return (mode == MODE_BLE) ? "Mode 1 (BLE)" : "Mode 2 (Thread)";
}

/* MODE button ("sw0" in this board's devicetree - see mesh_feather.dtsi,
 * shared by particle_argon/boron/xenon, where sw0 = &mode_button and the
 * silkscreen/docs across this project call it "MODE"). Read as a plain
 * GPIO rather than through the input subsystem, matching the pattern
 * already used for this exact alias in
 * zephyr/samples/net/openthread/coap/src/button.c in this checkout.
 */
#define MODE_BUTTON_NODE DT_ALIAS(sw0)
#if !DT_NODE_HAS_STATUS_OKAY(MODE_BUTTON_NODE)
#error "particle_xenon devicetree is missing the sw0 (MODE button) alias"
#endif
static const struct gpio_dt_spec mode_button = GPIO_DT_SPEC_GET(MODE_BUTTON_NODE, gpios);

/**
 * Fast, one-shot poll of the MODE button at boot. Deliberately not
 * interrupt-driven or debounced with a delay loop - by the time main() runs
 * this early, a physically held button reads as a stable level, and this
 * needs to stay quick since it sits ahead of everything else in main().
 *
 * gpio_pin_get_dt() already accounts for GPIO_ACTIVE_LOW from the
 * devicetree, so a return value of 1 means "pressed" regardless of the
 * button's electrical polarity.
 */
static bool mode_button_is_held(void)
{
	int err;

	if (!gpio_is_ready_dt(&mode_button)) {
		LOG_ERR("MODE button GPIO not ready; assuming not held");
		return false;
	}

	err = gpio_pin_configure_dt(&mode_button, GPIO_INPUT);
	if (err) {
		LOG_ERR("Failed to configure MODE button pin (err %d); assuming not held", err);
		return false;
	}

	return gpio_pin_get_dt(&mode_button) == 1;
}

/* ===========================================================================
 * 3. Shared sensor sampling
 * ===========================================================================
 * On-chip die temperature and VDD rail sensing are kept as small, separate
 * functions so neither mode's wireless plumbing ever has to know how a
 * value was produced - it just gets a filled-in telemetry_payload. Used
 * identically by both Mode 1 and Mode 2 via the single sampling thread in
 * section 6.
 */

/* nRF52840 on-chip die temperature sensor. Bound via the standard
 * "nordic,nrf-temp" devicetree node, which is already present (status =
 * "okay") in the SoC's own dtsi - no board overlay needed.
 */
static const struct device *const temp_dev = DEVICE_DT_GET_ONE(nordic_nrf_temp);

/* SAADC peripheral. The particle_xenon board dts already enables this node
 * (it backs the Feather ADC pins), so it is available without an overlay.
 * We add one extra software-only channel here (id 0) wired to the SAADC's
 * internal VDD tap rather than an external pin.
 */
static const struct device *const adc_dev = DEVICE_DT_GET(DT_NODELABEL(adc));

#define VDD_ADC_CHANNEL_ID 0
#define VDD_ADC_RESOLUTION_BITS 12
#define VDD_ADC_GAIN ADC_GAIN_1_6
#define VDD_ADC_REF_MV 600 /* nRF52 series internal reference is 0.6V */

/**
 * One-time setup of the VDD-sensing ADC channel. Must run before the first
 * sample_vdd_millivolts() call.
 *
 * Gain 1/6 against the 0.6V internal reference gives a measurable range of
 * 0 - 3.6V, which comfortably covers the board's regulated ~3.3V rail.
 */
static int vdd_adc_channel_init(void)
{
	static const struct adc_channel_cfg vdd_channel_cfg = {
		.gain = VDD_ADC_GAIN,
		.reference = ADC_REF_INTERNAL,
		.acquisition_time = ADC_ACQ_TIME_DEFAULT,
		.channel_id = VDD_ADC_CHANNEL_ID,
		.input_positive = NRF_SAADC_VDD,
	};

	if (!device_is_ready(adc_dev)) {
		LOG_ERR("SAADC device not ready");
		return -ENODEV;
	}

	return adc_channel_setup(adc_dev, &vdd_channel_cfg);
}

/**
 * Sample the nRF52840's on-chip die temperature sensor and convert the
 * result to signed centi-degrees Celsius (e.g. 23.50C -> 2350) for the
 * wire format.
 */
static int sample_die_temperature(int16_t *temp_centi_c)
{
	struct sensor_value val;
	int err;

	if (!device_is_ready(temp_dev)) {
		LOG_ERR("Die temperature sensor not ready");
		return -ENODEV;
	}

	err = sensor_sample_fetch(temp_dev);
	if (err) {
		LOG_ERR("temp: sensor_sample_fetch failed (err %d)", err);
		return err;
	}

	err = sensor_channel_get(temp_dev, SENSOR_CHAN_DIE_TEMP, &val);
	if (err) {
		LOG_ERR("temp: sensor_channel_get failed (err %d)", err);
		return err;
	}

	/* struct sensor_value represents val1 + val2/1e6 degrees C. Multiply
	 * the whole-degree part by 100 and the fractional (millionths) part
	 * by 1/10000 to land in centi-degrees.
	 */
	*temp_centi_c = (int16_t)(val.val1 * 100 + val.val2 / 10000);

	return 0;
}

/**
 * Sample the SAADC's internal VDD tap and convert to millivolts.
 *
 * Deliberately labeled "vdd_mv", not "battery voltage": on the Xenon this
 * channel reads the regulated ~3.3V rail coming out of the onboard LDO, not
 * the raw LiPo cell voltage (that would require the board's separate
 * resistor-divider "vbatt" analog input instead).
 */
static int sample_vdd_millivolts(uint16_t *vdd_mv)
{
	int16_t raw_sample = 0;
	int32_t mv;
	int err;
	struct adc_sequence sequence = {
		.channels = BIT(VDD_ADC_CHANNEL_ID),
		.buffer = &raw_sample,
		.buffer_size = sizeof(raw_sample),
		.resolution = VDD_ADC_RESOLUTION_BITS,
	};

	err = adc_read(adc_dev, &sequence);
	if (err) {
		LOG_WRN("vdd: adc_read failed (err %d)", err);
		return err;
	}

	mv = raw_sample;
	err = adc_raw_to_millivolts(VDD_ADC_REF_MV, VDD_ADC_GAIN,
				     VDD_ADC_RESOLUTION_BITS, &mv);
	if (err) {
		LOG_WRN("vdd: raw-to-millivolts conversion failed (err %d)", err);
		return err;
	}

	*vdd_mv = (uint16_t)CLAMP(mv, 0, UINT16_MAX);

	return 0;
}

/* ===========================================================================
 * 4. Mode 1: BLE
 * ===========================================================================
 * Unchanged from the original BLE-only xenon_sensor, except that bt_enable()
 * and start_advertising() are now only ever invoked from main() when
 * current_mode == MODE_BLE (see section 7). The GATT service/connection
 * callback *registrations* below are static declarations, not radio
 * activity - they cost nothing and stay compiled in for both modes, but
 * nothing is transmitted unless bt_enable() actually runs.
 */

/* Custom GATT service: "Xenon Sensor Telemetry". UUIDs are fixed by the
 * portfolio spec so the argon_gateway central can be written against them
 * independently. BT_UUID_128_ENCODE() takes a standard UUID string with the
 * hyphens replaced by commas and 0x prefixes added, so these two lines are
 * a direct transcription of:
 *
 *   Service:        a3f8c2d0-6b1e-4a7f-9c3d-8e2b5f1a9d40
 *   Characteristic:  a3f8c2d1-6b1e-4a7f-9c3d-8e2b5f1a9d40
 */
#define BT_UUID_XENON_SERVICE_VAL \
	BT_UUID_128_ENCODE(0xa3f8c2d0, 0x6b1e, 0x4a7f, 0x9c3d, 0x8e2b5f1a9d40)
#define BT_UUID_XENON_TELEMETRY_VAL \
	BT_UUID_128_ENCODE(0xa3f8c2d1, 0x6b1e, 0x4a7f, 0x9c3d, 0x8e2b5f1a9d40)

static const struct bt_uuid_128 xenon_service_uuid =
	BT_UUID_INIT_128(BT_UUID_XENON_SERVICE_VAL);
static const struct bt_uuid_128 xenon_telemetry_uuid =
	BT_UUID_INIT_128(BT_UUID_XENON_TELEMETRY_VAL);

/* Latest telemetry snapshot. Written by the sampling thread once per
 * SAMPLE_PERIOD; read by the Bluetooth host thread whenever a central issues
 * a GATT Read Request against the characteristic. Guarded by telemetry_lock
 * because it is a multi-field struct and Zephyr gives no atomicity guarantee
 * for that across threads. (In Mode 2 this is still written each cycle but
 * nothing ever reads it via GATT, since the Bluetooth host is never
 * started.)
 */
static struct telemetry_payload latest_telemetry;
static K_MUTEX_DEFINE(telemetry_lock);

/* Tracks whether a central has enabled notifications (written the CCC
 * "notify" bit). Updated only from the Bluetooth host thread's CCC
 * config-changed callback, read only by the sampling thread before it calls
 * bt_gatt_notify() - single writer, single reader, so a plain bool is fine.
 */
static bool notifications_enabled;

/* Released once by main() after the selected mode's stack is fully up
 * (BLE: bt_enable() + advertising; Thread: openthread_run() + UDP socket),
 * so the sampling thread never touches the sensors/ADC/radio stack before
 * that mode is actually ready.
 */
static K_SEM_DEFINE(stack_ready_sem, 0, 1);

/**
 * GATT read callback for the telemetry characteristic. Takes a mutex-guarded
 * snapshot so a central polling via Read (instead of, or in addition to,
 * notifications) always gets a consistent 8-byte struct rather than a
 * value the sampling thread is mid-update on.
 */
static ssize_t read_telemetry(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			       void *buf, uint16_t len, uint16_t offset)
{
	struct telemetry_payload snapshot;

	k_mutex_lock(&telemetry_lock, K_FOREVER);
	snapshot = latest_telemetry;
	k_mutex_unlock(&telemetry_lock);

	return bt_gatt_attr_read(conn, attr, buf, len, offset, &snapshot, sizeof(snapshot));
}

/**
 * CCC (Client Characteristic Configuration) descriptor callback. Fires
 * whenever a central subscribes/unsubscribes from notifications on the
 * telemetry characteristic. This is the only thing that gates whether the
 * sampling thread actually calls bt_gatt_notify() each cycle.
 */
static void telemetry_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	notifications_enabled = (value == BT_GATT_CCC_NOTIFY);

	LOG_INF("Central %s telemetry notifications",
		notifications_enabled ? "subscribed to" : "unsubscribed from");
}

/* Primary service + one read/notify characteristic + its CCC descriptor.
 * Attribute layout after this macro expands is fixed by Zephyr's GATT
 * table conventions: [0] service, [1] characteristic declaration,
 * [2] characteristic value, [3] CCC descriptor - hence xenon_svc.attrs[2]
 * below is the value attribute bt_gatt_notify() needs.
 */
BT_GATT_SERVICE_DEFINE(xenon_svc,
	BT_GATT_PRIMARY_SERVICE(&xenon_service_uuid),
	BT_GATT_CHARACTERISTIC(&xenon_telemetry_uuid.uuid,
				BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
				BT_GATT_PERM_READ,
				read_telemetry, NULL, NULL),
	BT_GATT_CCC(telemetry_ccc_changed,
		    BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
);

#define TELEMETRY_VALUE_ATTR (&xenon_svc.attrs[2])

/* Advertising + connection lifecycle. Scope decision: once a central
 * connects we stay connected persistently (we do not disconnect/re-
 * advertise on a timer). We only resume advertising after an actual
 * disconnect event.
 */

static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	/* Advertise our custom 128-bit service UUID so a central can
	 * filter-scan specifically for this sensor.
	 */
	BT_DATA_BYTES(BT_DATA_UUID128_ALL, BT_UUID_XENON_SERVICE_VAL),
};

static const struct bt_data sd[] = {
	BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME, sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

static struct bt_conn *current_conn;

static int start_advertising(void)
{
	int err;

	err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_1, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
	if (err) {
		LOG_ERR("Advertising failed to start (err %d)", err);
		return err;
	}

	LOG_INF("Advertising started as \"%s\"", CONFIG_BT_DEVICE_NAME);
	return 0;
}

static void connected(struct bt_conn *conn, uint8_t err)
{
	if (err) {
		LOG_ERR("Connection failed (err 0x%02x)", err);
		return;
	}

	current_conn = bt_conn_ref(conn);
	LOG_INF("Central connected");
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	LOG_INF("Central disconnected (reason 0x%02x)", reason);

	if (current_conn) {
		bt_conn_unref(current_conn);
		current_conn = NULL;
	}
	notifications_enabled = false;

	/* Only re-enter advertising state now that we're actually alone
	 * again - while connected we intentionally do not advertise.
	 */
	(void)start_advertising();
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
};

/**
 * Bring up Mode 1: enable the Bluetooth host and start advertising. Only
 * ever called from main() when current_mode == MODE_BLE - this is the one
 * and only place bt_enable() is called anywhere in this file.
 */
static int ble_mode_start(void)
{
	int err;

	err = bt_enable(NULL);
	if (err) {
		LOG_ERR("Bluetooth init failed (err %d)", err);
		return err;
	}
	LOG_INF("Bluetooth initialized");

	return start_advertising();
}

/* ===========================================================================
 * 5. Mode 2: Thread
 * ===========================================================================
 * Forms/joins a Thread network using the hardcoded credentials in prj.conf
 * (CONFIG_OPENTHREAD_PANID/XPANID/NETWORKKEY/CHANNEL/NETWORK_NAME) and sends
 * the same telemetry struct as Mode 1, but over UDP multicast instead of
 * BLE GATT notify. Real commissioning (joiner/PSKd flow) is explicitly out
 * of scope for this demo - see the module header.
 *
 * CONFIG_OPENTHREAD_MANUAL_START keeps the network administratively down
 * until openthread_run() is called explicitly below, so Thread traffic only
 * ever happens when this mode is selected - mirroring how bt_enable() is
 * the sole gate for Mode 1's radio activity.
 */

/* Realm-local (mesh-wide, not routed beyond the Thread network) multicast
 * group and port for telemetry. Arbitrary but fixed, and deliberately not
 * one of Thread's reserved multicast addresses (ff03::1 "all Thread nodes",
 * ff03::2 "all Thread routers") so a listener has to explicitly join this
 * group to see our traffic. There is no border router or listener
 * implemented anywhere in this project yet - this only needs to be a real,
 * correct transmission for now. Document these for whoever builds that
 * listener later:
 *   Multicast address: ff03::abcd
 *   Port:               4242
 */
#define THREAD_TELEMETRY_MCAST_ADDR "ff03::abcd"
#define THREAD_TELEMETRY_MCAST_PORT 4242

/* Default IPv6 multicast hop limit in Zephyr is 1 (see
 * CONFIG_NET_INITIAL_MCAST_HOP_LIMIT / net_context.c), which would prevent
 * this traffic from crossing more than one Thread mesh hop. Raised here so
 * it can reach routers/leaders elsewhere in the mesh, not just this node's
 * immediate parent.
 */
#define THREAD_TELEMETRY_MCAST_HOPS 8

static int thread_udp_sock = -1;
static struct net_sockaddr_in6 thread_telemetry_dst;

/**
 * OpenThread state-changed callback, registered with the module's own
 * multi-consumer callback list (openthread_state_changed_callback_register)
 * rather than otSetStateChangedCallback() directly, since that single-slot
 * API is already claimed internally by the L2 driver. Only handles role
 * transitions, which is what "joined the network" boils down to in
 * OpenThread: DETACHED -> CHILD/ROUTER/LEADER means attached.
 */
static void thread_state_changed(otChangedFlags flags, void *context)
{
	ARG_UNUSED(context);

	if (!(flags & OT_CHANGED_THREAD_ROLE)) {
		return;
	}

	otInstance *ot = openthread_get_default_instance();
	otDeviceRole role = otThreadGetDeviceRole(ot);

	LOG_INF("Thread role changed: %s", otThreadDeviceRoleToString(role));

	switch (role) {
	case OT_DEVICE_ROLE_CHILD:
	case OT_DEVICE_ROLE_ROUTER:
	case OT_DEVICE_ROLE_LEADER:
		LOG_INF("Thread network join/attach succeeded (role=%s)",
			otThreadDeviceRoleToString(role));
		break;
	case OT_DEVICE_ROLE_DETACHED:
		LOG_WRN("Thread network detached; still attempting to (re)join");
		break;
	case OT_DEVICE_ROLE_DISABLED:
	default:
		break;
	}
}

static struct openthread_state_changed_callback thread_state_cb = {
	.otCallback = thread_state_changed,
};

/**
 * Bring up Mode 2: register role-change logging, join the Thread network
 * (this is the one and only place openthread_run() is called anywhere in
 * this file), then open the UDP socket used for telemetry multicast. Only
 * ever called from main() when current_mode == MODE_THREAD.
 */
static int thread_mode_start(void)
{
	int err;
	int hops = THREAD_TELEMETRY_MCAST_HOPS;

	LOG_INF("Starting Thread stack: PAN ID 0x%04x, channel %d, network \"%s\"",
		CONFIG_OPENTHREAD_PANID, CONFIG_OPENTHREAD_CHANNEL,
		CONFIG_OPENTHREAD_NETWORK_NAME);

	(void)openthread_state_changed_callback_register(&thread_state_cb);

	err = openthread_run();
	if (err) {
		LOG_ERR("Thread network join/attach attempt failed to start (err %d)", err);
		return err;
	}
	LOG_INF("Thread network join/attach attempt started");

	thread_udp_sock = zsock_socket(NET_AF_INET6, NET_SOCK_DGRAM, NET_IPPROTO_UDP);
	if (thread_udp_sock < 0) {
		LOG_ERR("Failed to create UDP socket (errno %d)", errno);
		return -errno;
	}

	err = zsock_setsockopt(thread_udp_sock, NET_IPPROTO_IPV6, ZSOCK_IPV6_MULTICAST_HOPS,
				&hops, sizeof(hops));
	if (err) {
		LOG_WRN("Failed to raise multicast hop limit (errno %d); using default", errno);
	}

	memset(&thread_telemetry_dst, 0, sizeof(thread_telemetry_dst));
	thread_telemetry_dst.sin6_family = NET_AF_INET6;
	thread_telemetry_dst.sin6_port = net_htons(THREAD_TELEMETRY_MCAST_PORT);
	err = zsock_inet_pton(NET_AF_INET6, THREAD_TELEMETRY_MCAST_ADDR,
			       &thread_telemetry_dst.sin6_addr);
	if (err != 1) {
		LOG_ERR("Failed to parse multicast address \"%s\"", THREAD_TELEMETRY_MCAST_ADDR);
		zsock_close(thread_udp_sock);
		thread_udp_sock = -1;
		return -EINVAL;
	}

	LOG_INF("UDP telemetry target [%s]:%d ready",
		THREAD_TELEMETRY_MCAST_ADDR, THREAD_TELEMETRY_MCAST_PORT);

	return 0;
}

/**
 * Send one telemetry sample as an 8-byte UDP multicast datagram. Mirrors
 * the logging style of Mode 1's bt_gatt_notify() call site.
 */
static void thread_send_telemetry(const struct telemetry_payload *payload)
{
	ssize_t sent;

	if (thread_udp_sock < 0) {
		return;
	}

	sent = zsock_sendto(thread_udp_sock, payload, sizeof(*payload), 0,
			     (struct net_sockaddr *)&thread_telemetry_dst,
			     sizeof(thread_telemetry_dst));
	if (sent < 0) {
		LOG_WRN("UDP multicast send failed for sample #%u (errno %d)",
			payload->seq, errno);
		return;
	}

	LOG_INF("UDP multicast sent: seq=%u temp=%d vdd=%u -> [%s]:%d",
		payload->seq, payload->temp_centi_c, payload->vdd_mv,
		THREAD_TELEMETRY_MCAST_ADDR, THREAD_TELEMETRY_MCAST_PORT);
}

/* ===========================================================================
 * 6. Sampling thread
 * ===========================================================================
 * Runs as its own K_THREAD_DEFINE() context - not the system workqueue, not
 * inline in main(). This keeps sensor I/O and telemetry cadence fully
 * decoupled from whatever main() or the radio stack's own thread(s) happen
 * to be doing, which is the standard Zephyr shape for a periodic producer.
 * Identical in both modes except for the last step: notify over BLE, or
 * send over UDP multicast.
 */

#define SAMPLING_THREAD_STACK_SIZE 2048
#define SAMPLING_THREAD_PRIORITY 7 /* Preemptible; distinct from the BT host / OT work queue threads */

static void sampling_thread_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	uint32_t seq = 0;
	int err;

	/* Do not touch sensors/ADC/radio until the selected mode's stack has
	 * finished bringing itself up in main(). This is the thread's only
	 * synchronization point with main(); after this it runs entirely on
	 * its own clock.
	 */
	k_sem_take(&stack_ready_sem, K_FOREVER);

	err = vdd_adc_channel_init();
	if (err) {
		LOG_ERR("VDD ADC channel init failed (err %d); vdd_mv will be stale", err);
	}

	LOG_INF("Sampling thread started (period=%ds, mode=%s)", SAMPLE_PERIOD_S,
		mode_name(current_mode));

	while (1) {
		/* This k_sleep() is the actual power-saving mechanism for this
		 * application: Zephyr's kernel is tickless, so for the full
		 * 5 seconds between samples the CPU is free to drop into its
		 * deepest available idle state automatically. No extra
		 * Kconfig, no manual WFI/sleep-mode juggling required - the
		 * idle time falls out naturally from this thread simply not
		 * being runnable.
		 */
		k_sleep(SAMPLE_PERIOD);

		int16_t temp_centi_c = 0;
		uint16_t vdd_mv = 0;
		struct telemetry_payload snapshot;

		(void)sample_die_temperature(&temp_centi_c);
		(void)sample_vdd_millivolts(&vdd_mv);
		seq++;

		k_mutex_lock(&telemetry_lock, K_FOREVER);
		latest_telemetry.temp_centi_c = temp_centi_c;
		latest_telemetry.vdd_mv = vdd_mv;
		latest_telemetry.seq = seq;
		snapshot = latest_telemetry;
		k_mutex_unlock(&telemetry_lock);

		LOG_INF("sample #%u: temp=%d.%02u C  vdd=%u mV",
			seq,
			temp_centi_c / 100,
			(unsigned int)(temp_centi_c < 0 ? -temp_centi_c % 100 : temp_centi_c % 100),
			vdd_mv);

		if (current_mode == MODE_BLE) {
			if (!notifications_enabled) {
				LOG_INF("No subscriber; skipping notify for sample #%u", seq);
				continue;
			}

			err = bt_gatt_notify(NULL, TELEMETRY_VALUE_ATTR, &snapshot,
					      sizeof(snapshot));
			if (err) {
				LOG_WRN("Notify failed for sample #%u (err %d)", seq, err);
			} else {
				LOG_INF("Notified subscriber: seq=%u temp=%d vdd=%u",
					snapshot.seq, snapshot.temp_centi_c, snapshot.vdd_mv);
			}
		} else {
			thread_send_telemetry(&snapshot);
		}
	}
}

K_THREAD_DEFINE(sampling_tid, SAMPLING_THREAD_STACK_SIZE, sampling_thread_entry,
		 NULL, NULL, NULL, SAMPLING_THREAD_PRIORITY, 0, 0);

/* ===========================================================================
 * 7. Entry point
 * ===========================================================================
 * main() resolves the mode (checking the MODE button first), then brings up
 * exactly one radio stack, then hands off to the sampling thread above. All
 * recurring work happens on that thread and inside the radio stack's own
 * thread(s) - main() itself has nothing left to do once it returns.
 */
/* With a native USB-CDC console (see boards/particle_xenon.overlay), early
 * printk()/LOG_INF output before a host has actually opened the serial port
 * can be silently dropped -- there's no DTR asserted yet. Wait for DTR with
 * a bounded timeout so the app still proceeds (both modes are useful even
 * with nobody watching the console) if nothing ever opens the port. Same
 * idiom as argon_gateway's wait_for_console_dtr().
 */
#define CONSOLE_DTR_WAIT_TIMEOUT_MS 3000
#define CONSOLE_DTR_POLL_INTERVAL_MS 100

static void wait_for_console_dtr(void)
{
	const struct device *const console_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
	uint32_t dtr = 0;
	int waited_ms = 0;

	if (!device_is_ready(console_dev)) {
		return;
	}

	while (!dtr && waited_ms < CONSOLE_DTR_WAIT_TIMEOUT_MS) {
		uart_line_ctrl_get(console_dev, UART_LINE_CTRL_DTR, &dtr);
		k_sleep(K_MSEC(CONSOLE_DTR_POLL_INTERVAL_MS));
		waited_ms += CONSOLE_DTR_POLL_INTERVAL_MS;
	}
}

int main(void)
{
	int err;
	enum device_mode mode;

	wait_for_console_dtr();

	err = mode_storage_init();
	if (err) {
		/* Can't safely honor a MODE button press without durable
		 * storage to persist it to (that would risk a reboot loop),
		 * so just fall back to Mode 1 for this boot and skip the
		 * button check entirely.
		 */
		LOG_ERR("Mode storage init failed (err %d); defaulting to Mode 1 (BLE) "
			"for this boot only", err);
		mode = MODE_BLE;
	} else {
		mode = mode_load();

		if (mode_button_is_held()) {
			enum device_mode next = (mode == MODE_BLE) ? MODE_THREAD : MODE_BLE;

			LOG_INF("MODE button held at boot: switching %s -> %s",
				mode_name(mode), mode_name(next));

			err = mode_store(next);
			if (err) {
				LOG_ERR("Failed to persist new mode (err %d); staying in %s",
					err, mode_name(mode));
			} else {
				LOG_INF("Rebooting to apply %s...", mode_name(next));
				/* Give the log backend a moment to flush the
				 * message above over the console UART before
				 * the reset tears everything down.
				 */
				k_sleep(K_MSEC(50));
				sys_reboot(SYS_REBOOT_COLD);
				/* unreachable */
			}
		}
	}

	current_mode = mode;
	LOG_INF("Booting in %s", mode_name(mode));

	if (mode == MODE_BLE) {
		err = ble_mode_start();
	} else {
		err = thread_mode_start();
	}

	if (err) {
		LOG_ERR("Failed to start %s (err %d)", mode_name(mode), err);
		return 0;
	}

	/* Let the sampling thread proceed now that the selected mode's stack
	 * is up.
	 */
	k_sem_give(&stack_ready_sem);

	return 0;
}
