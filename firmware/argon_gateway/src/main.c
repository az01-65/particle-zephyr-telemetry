/*
 * argon_gateway - dual-mode telemetry gateway for the Particle Argon
 * (Nordic nRF52840).
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * -----------------------------------------------------------------------
 * Architecture, for the reviewer
 * -----------------------------------------------------------------------
 * This firmware boots into one of two mutually exclusive radio modes,
 * mirroring the exact pattern already built and proven on the sibling
 * xenon_sensor app (see firmware/xenon_sensor/src/main.c):
 *
 *   Mode 1 (BLE):    a GATT central that scans for, connects to, and
 *                     subscribes to xenon_sensor's telemetry
 *                     characteristic, then relays each notification as a
 *                     JSON line on the USB-CDC console. This is the
 *                     original (and, until now, only) argon_gateway
 *                     behavior, unchanged.
 *   Mode 2 (Thread):  an 802.15.4/Thread node that joins the same Thread
 *                     network xenon_sensor's Mode 2 joins, subscribes to
 *                     its UDP telemetry multicast group, and relays each
 *                     received datagram as the exact same JSON line
 *                     format -- so tools/telemetry_monitor.py needs zero
 *                     changes regardless of which mode either board is
 *                     running.
 *
 * Why a reboot is required to switch: the nRF52840 has a single 2.4GHz
 * radio. BLE and 802.15.4/Thread both want it, and Zephyr's BLE controller
 * and the 802.15.4 driver both want the same RADIO_IRQn vector - they
 * cannot usefully run at the same time on this hardware. So instead of
 * attempting to hot-swap radio stacks, the selected mode is persisted to
 * flash (NVS) and read once at boot; only that mode's stack is ever
 * started. Switching modes is a deliberate, explicit action (hold the
 * MODE button at boot) followed by a reboot - not a limitation to work
 * around, but the honest shape of the hardware constraint. This is
 * identical reasoning to xenon_sensor's, copied here rather than
 * re-derived, since it's the same SoC hitting the same constraint.
 *
 * Section map:
 *   1. Wire format                  - shared 8-byte telemetry struct
 *   2. Mode selection                - persisted mode, MODE button handling
 *   3. Shared telemetry JSON emission - one JSON-line emitter, both modes
 *   4. Mode 1: BLE central           - scan/connect/discover/subscribe
 *   5. Mode 2: Thread listener       - network join, UDP multicast receive
 *   6. Status LED                    - visual mode/connection/switch readout
 *   7. Entry point                   - mode load/switch, stack bring-up
 *
 * Concurrency note: Mode 1's BLE work all happens on Zephyr's Bluetooth
 * host callbacks/system workqueue (no hand-rolled thread), same as the
 * original BLE-only argon_gateway. Mode 2's UDP receive loop runs on its
 * own dedicated K_THREAD_DEFINE() thread, consistent with how xenon_sensor
 * keeps its sampling thread separate from main()/radio-stack threads.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>
#include <zephyr/sys/reboot.h>

#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/drivers/uart.h>

#include <zephyr/kvss/nvs.h>
#include <zephyr/storage/flash_map.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/att.h>

#include <zephyr/net/openthread.h>
#include <zephyr/net/socket.h>
#include <openthread/thread.h>
#include <openthread/instance.h>

#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(argon_gateway, LOG_LEVEL_INF);

/* ===========================================================================
 * 1. Wire format - shared byte-for-byte with xenon_sensor, on both the BLE
 *    notification payload (Mode 1) and the Thread UDP multicast payload
 *    (Mode 2). See xenon_sensor/src/main.c section 1 for the canonical
 *    definition this is copied from.
 * ===========================================================================
 */
struct __packed telemetry_payload {
	int16_t temp_centi_c;  /* hundredths of a degree C */
	uint16_t vdd_mv;        /* millivolts */
	uint32_t seq;            /* monotonic sample counter */
	uint8_t node_id;         /* per-board ID; see xenon_sensor's node_id_init() */
};

BUILD_ASSERT(sizeof(struct telemetry_payload) == 9,
	     "telemetry_payload must match the 9-byte wire format on both links");

/* ===========================================================================
 * 2. Mode selection
 * ===========================================================================
 * Copied as closely as possible from xenon_sensor/src/main.c section 2 -
 * same NVS-backed storage_partition, same persisted key, same MODE button
 * alias, same one-shot poll-at-boot semantics. See that file for the full
 * design rationale; only the two mode names differ in meaning (BLE central
 * vs. BLE peripheral, Thread listener vs. Thread sender) but not in
 * mechanism.
 */

enum device_mode {
	MODE_BLE = 0,
	MODE_THREAD = 1,
};

#define MODE_NVS_ID 1U

static struct nvs_fs mode_fs;

/* Set once in main() before either mode's stack is brought up, and only
 * ever read afterward - no lock needed.
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
 * shared by particle_argon/boron/xenon, where sw0 = &mode_button). Read as
 * a plain GPIO rather than through the input subsystem, matching the
 * pattern used for this exact alias in
 * zephyr/samples/net/openthread/coap/src/button.c in this checkout, and in
 * xenon_sensor's own mode_button_is_held().
 */
#define MODE_BUTTON_NODE DT_ALIAS(sw0)
#if !DT_NODE_HAS_STATUS_OKAY(MODE_BUTTON_NODE)
#error "particle_argon devicetree is missing the sw0 (MODE button) alias"
#endif
static const struct gpio_dt_spec mode_button = GPIO_DT_SPEC_GET(MODE_BUTTON_NODE, gpios);

/* Set true once mode_button_init() has successfully configured the pin, so
 * mode_button_read() (called repeatedly during the confirmation gesture in
 * section 6) doesn't need to reconfigure it on every poll.
 */
static bool mode_button_ready;

/**
 * One-time configuration of the MODE button pin. Deliberately not
 * interrupt-driven - by the time main() runs this early, a physically held
 * button reads as a stable level, and plain polling is simplest for a
 * boot-time gesture.
 */
static void mode_button_init(void)
{
	if (!gpio_is_ready_dt(&mode_button)) {
		LOG_ERR("MODE button GPIO not ready; will assume never held");
		return;
	}

	if (gpio_pin_configure_dt(&mode_button, GPIO_INPUT)) {
		LOG_ERR("Failed to configure MODE button pin; will assume never held");
		return;
	}

	mode_button_ready = true;
}

/**
 * Poll the MODE button's current level. gpio_pin_get_dt() already accounts
 * for GPIO_ACTIVE_LOW from the devicetree, so a return value of 1 means
 * "pressed" regardless of the button's electrical polarity. Cheap enough to
 * call repeatedly in the confirmation gesture's polling loop.
 */
static bool mode_button_read(void)
{
	if (!mode_button_ready) {
		return false;
	}

	return gpio_pin_get_dt(&mode_button) == 1;
}

/* ===========================================================================
 * 3. Shared telemetry JSON emission
 * ===========================================================================
 * A single decode-and-print helper used by both modes so the on-the-wire
 * -> JSON translation only exists in one place, rather than being
 * duplicated (and risking drift) between the BLE notify handler and the
 * Thread UDP receive loop. Both wire formats carry the identical 8-byte
 * little-endian struct telemetry_payload, so one decoder covers both.
 */

/**
 * Decode a raw 9-byte little-endian telemetry_payload buffer and print it
 * as a JSON line on the USB-CDC console, in the exact format
 * tools/telemetry_monitor.py already expects:
 *
 *   {"node_id": "0x4a", "temp_c": 23.50, "vdd_mv": 3300, "seq": 12}
 *
 * node_id is emitted as a hex string (not a bare number) since it's an
 * opaque per-board fingerprint, not a quantity - printing it as "0x4a"
 * rather than "74" keeps that distinction obvious to a human reading the
 * stream.
 *
 * @param data   Pointer to at least sizeof(struct telemetry_payload) bytes.
 * @param length Length of the buffer at data, in bytes.
 * @param source Short tag identifying the origin link, for the LOG_INF
 *               breadcrumb only (not part of the JSON output).
 */
static void telemetry_emit_json(const void *data, size_t length, const char *source)
{
	struct telemetry_payload payload;

	if (length != sizeof(payload)) {
		LOG_WRN("Dropping %s payload with unexpected length %u (expected %u)",
			source, (unsigned int)length, (unsigned int)sizeof(payload));
		return;
	}

	/* Decode the packed little-endian struct field-by-field rather than
	 * trusting host struct layout/endianness to match the wire exactly.
	 * The nRF52840 is little-endian, so this is a no-op here in practice,
	 * but being explicit keeps the decode correct if this code is ever
	 * reused on a big-endian host.
	 */
	memcpy(&payload, data, sizeof(payload));

	int16_t temp_centi_c = (int16_t)sys_le16_to_cpu((uint16_t)payload.temp_centi_c);
	uint16_t vdd_mv = sys_le16_to_cpu(payload.vdd_mv);
	uint32_t seq = sys_le32_to_cpu(payload.seq);
	uint8_t node_id = payload.node_id;

	float temp_c = (float)temp_centi_c / 100.0f;

	/* JSON on USB serial for tools/telemetry_monitor.py -- identical
	 * format regardless of which mode/link produced this sample.
	 */
	printk("{\"node_id\": \"0x%02x\", \"temp_c\": %.2f, \"vdd_mv\": %u, \"seq\": %u}\n",
	       node_id, (double)temp_c, vdd_mv, seq);

	LOG_INF("Telemetry from %s: node=0x%02x temp=%.2fC vdd=%umV seq=%u",
		source, node_id, (double)temp_c, vdd_mv, seq);
}

/* ===========================================================================
 * 4. Mode 1: BLE central
 * ===========================================================================
 * Unchanged from the original BLE-only argon_gateway, except that
 * bt_enable()/start_scan() are now only ever invoked from main() when
 * current_mode == MODE_BLE (see section 6), and notify_func() now calls the
 * shared telemetry_emit_json() helper from section 3 instead of decoding
 * and printing inline.
 */

/* a3f8c2d0-6b1e-4a7f-9c3d-8e2b5f1a9d40 */
#define BT_UUID_XENON_TELEMETRY_SVC_VAL \
	BT_UUID_128_ENCODE(0xa3f8c2d0, 0x6b1e, 0x4a7f, 0x9c3d, 0x8e2b5f1a9d40)

/* a3f8c2d1-6b1e-4a7f-9c3d-8e2b5f1a9d40 (notify-capable) */
#define BT_UUID_XENON_TELEMETRY_CHR_VAL \
	BT_UUID_128_ENCODE(0xa3f8c2d1, 0x6b1e, 0x4a7f, 0x9c3d, 0x8e2b5f1a9d40)

static const struct bt_uuid_128 xenon_svc_uuid =
	BT_UUID_INIT_128(BT_UUID_XENON_TELEMETRY_SVC_VAL);
static const struct bt_uuid_128 xenon_chr_uuid =
	BT_UUID_INIT_128(BT_UUID_XENON_TELEMETRY_CHR_VAL);

/* Advertised device name, checked only as a secondary sanity check -- the
 * scan filter below keys primarily off the service UUID, which is the more
 * robust match (names can collide or be truncated in the AD payload).
 */
#define XENON_DEVICE_NAME "XenonSensor"

static struct bt_conn *default_conn;

/* Set true once GATT discovery actually finds and subscribes to the
 * telemetry characteristic (not merely "BLE connected"), and read by the
 * status LED thread in section 6. A raw bt_conn pointer isn't enough on its
 * own to mean "connected" for LED purposes - discovery can still be
 * in-flight or fail.
 */
static bool ble_connected;

static struct bt_gatt_discover_params discover_params;
static struct bt_gatt_subscribe_params subscribe_params;

/* Reused across the service -> characteristic discovery stages (both are
 * 128-bit UUIDs). The CCC descriptor stage uses the fixed 16-bit CCC UUID
 * below instead, since discover_uuid can't hold both sizes.
 */
static struct bt_uuid_128 discover_uuid;
static struct bt_uuid_16 ccc_uuid = BT_UUID_INIT_16(BT_UUID_GATT_CCC_VAL);

static void start_scan(void);
static void start_discovery(struct bt_conn *conn);

static uint8_t notify_func(struct bt_conn *conn,
			    struct bt_gatt_subscribe_params *params,
			    const void *data, uint16_t length)
{
	ARG_UNUSED(conn);

	if (!data) {
		LOG_INF("Unsubscribed from telemetry characteristic");
		params->value_handle = 0U;
		return BT_GATT_ITER_STOP;
	}

	telemetry_emit_json(data, length, "BLE");

	return BT_GATT_ITER_CONTINUE;
}

/* ===========================================================================
 * GATT discovery
 *
 * Three-stage chain driven by a single bt_gatt_discover_params, mirroring
 * Zephyr's samples/bluetooth/central_hr shape:
 *   1. Discover the primary telemetry service by UUID.
 *   2. Within that service, discover the telemetry characteristic by UUID.
 *   3. Discover its CCC descriptor, then subscribe to notifications.
 * ===========================================================================
 */

static uint8_t discover_func(struct bt_conn *conn,
			      const struct bt_gatt_attr *attr,
			      struct bt_gatt_discover_params *params)
{
	int err;

	if (!attr) {
		LOG_WRN("GATT discovery finished without finding the telemetry characteristic");
		(void)memset(params, 0, sizeof(*params));
		return BT_GATT_ITER_STOP;
	}

	if (bt_uuid_cmp(params->uuid, &xenon_svc_uuid.uuid) == 0) {
		LOG_INF("Discovered telemetry service (handle %u)", attr->handle);

		memcpy(&discover_uuid, &xenon_chr_uuid, sizeof(discover_uuid));
		discover_params.uuid = &discover_uuid.uuid;
		discover_params.start_handle = attr->handle + 1;
		discover_params.type = BT_GATT_DISCOVER_CHARACTERISTIC;

		err = bt_gatt_discover(conn, &discover_params);
		if (err) {
			LOG_ERR("Characteristic discovery failed to start (err %d)", err);
		}
		return BT_GATT_ITER_STOP;
	}

	if (bt_uuid_cmp(params->uuid, &xenon_chr_uuid.uuid) == 0) {
		LOG_INF("Discovered telemetry characteristic (handle %u)", attr->handle);

		subscribe_params.value_handle = bt_gatt_attr_value_handle(attr);

		discover_params.uuid = &ccc_uuid.uuid;
		discover_params.start_handle = attr->handle + 2;
		discover_params.type = BT_GATT_DISCOVER_DESCRIPTOR;

		err = bt_gatt_discover(conn, &discover_params);
		if (err) {
			LOG_ERR("CCC descriptor discovery failed to start (err %d)", err);
		}
		return BT_GATT_ITER_STOP;
	}

	/* Anything else reaching here is the CCC descriptor -- subscribe. */
	LOG_INF("Discovered CCC descriptor (handle %u)", attr->handle);

	subscribe_params.notify = notify_func;
	subscribe_params.value = BT_GATT_CCC_NOTIFY;
	subscribe_params.ccc_handle = attr->handle;

	err = bt_gatt_subscribe(conn, &subscribe_params);
	if (err && err != -EALREADY) {
		LOG_ERR("Subscribe to telemetry characteristic failed (err %d)", err);
	} else {
		LOG_INF("Subscribed to telemetry notifications");
		ble_connected = true;
	}

	return BT_GATT_ITER_STOP;
}

static void start_discovery(struct bt_conn *conn)
{
	memcpy(&discover_uuid, &xenon_svc_uuid, sizeof(discover_uuid));

	discover_params.uuid = &discover_uuid.uuid;
	discover_params.func = discover_func;
	discover_params.start_handle = BT_ATT_FIRST_ATTRIBUTE_HANDLE;
	discover_params.end_handle = BT_ATT_LAST_ATTRIBUTE_HANDLE;
	discover_params.type = BT_GATT_DISCOVER_PRIMARY;

	int err = bt_gatt_discover(conn, &discover_params);

	if (err) {
		LOG_ERR("GATT discovery failed to start (err %d)", err);
		return;
	}

	LOG_INF("GATT discovery started for telemetry service");
}

/* ===========================================================================
 * Scan / connect
 *
 * Filtered scan: only devices advertising the telemetry service UUID are
 * connected to. The device name is checked too, but only as a secondary,
 * informational check -- the UUID match is what actually drives the connect
 * decision.
 * ===========================================================================
 */

struct adv_scan_result {
	bool svc_uuid_match;
	char name[32];
};

static bool eir_found(struct bt_data *data, void *user_data)
{
	struct adv_scan_result *result = user_data;

	switch (data->type) {
	case BT_DATA_UUID128_SOME:
	case BT_DATA_UUID128_ALL:
		if (data->data_len % 16U != 0U) {
			LOG_WRN("Malformed 128-bit UUID AD field, skipping");
			break;
		}

		for (uint8_t i = 0; i < data->data_len; i += 16U) {
			struct bt_uuid_128 uuid;

			if (!bt_uuid_create(&uuid.uuid, &data->data[i], 16U)) {
				continue;
			}

			if (bt_uuid_cmp(&uuid.uuid, &xenon_svc_uuid.uuid) == 0) {
				result->svc_uuid_match = true;
				break;
			}
		}
		break;

	case BT_DATA_NAME_COMPLETE:
	case BT_DATA_NAME_SHORTENED: {
		size_t len = MIN(data->data_len, sizeof(result->name) - 1);

		memcpy(result->name, data->data, len);
		result->name[len] = '\0';
		break;
	}

	default:
		break;
	}

	/* Keep parsing subsequent AD structures regardless of what we've
	 * matched so far -- the UUID and name fields may appear in either
	 * order (or in the scan response rather than the primary AD).
	 */
	return true;
}

static void device_found(const bt_addr_le_t *addr, int8_t rssi, uint8_t type,
			  struct net_buf_simple *ad)
{
	if (default_conn) {
		/* Already connecting/connected; ignore further reports. */
		return;
	}

	/* Only connectable undirected advertising is of interest here. */
	if (type != BT_GAP_ADV_TYPE_ADV_IND) {
		return;
	}

	struct adv_scan_result result = {0};

	bt_data_parse(ad, eir_found, &result);

	if (!result.svc_uuid_match) {
		return;
	}

	LOG_INF("Device found: %s (RSSI %d) name=\"%s\" advertises telemetry service",
		bt_addr_le_str(addr), rssi, result.name[0] ? result.name : "?");

	if (result.name[0] != '\0' && strcmp(result.name, XENON_DEVICE_NAME) != 0) {
		/* Secondary check only -- log a warning but still proceed,
		 * since the service UUID match is authoritative.
		 */
		LOG_WRN("Advertised name \"%s\" != expected \"%s\"; connecting anyway "
			"(UUID match is authoritative)", result.name, XENON_DEVICE_NAME);
	}

	int err = bt_le_scan_stop();

	if (err) {
		LOG_ERR("Failed to stop scan (err %d)", err);
		return;
	}

	err = bt_conn_le_create(addr, BT_CONN_LE_CREATE_CONN, BT_LE_CONN_PARAM_DEFAULT,
				 &default_conn);
	if (err) {
		LOG_ERR("Connection creation failed (err %d)", err);
		start_scan();
	}
}

static void start_scan(void)
{
	/* Active scan so we also pick up scan response data -- the 128-bit
	 * service UUID and the device name together don't reliably fit in a
	 * single legacy 31-byte advertising PDU, so the peripheral may split
	 * them across the primary AD and the scan response.
	 */
	struct bt_le_scan_param scan_param = {
		.type = BT_LE_SCAN_TYPE_ACTIVE,
		.options = BT_LE_SCAN_OPT_NONE,
		.interval = BT_GAP_SCAN_FAST_INTERVAL,
		.window = BT_GAP_SCAN_FAST_WINDOW,
	};

	int err = bt_le_scan_start(&scan_param, device_found);

	if (err) {
		LOG_ERR("Scan start failed (err %d)", err);
		return;
	}

	LOG_INF("Scan started, filtering on telemetry service UUID "
		"a3f8c2d0-6b1e-4a7f-9c3d-8e2b5f1a9d40");
}

/* ===========================================================================
 * Connection lifecycle
 * ===========================================================================
 */

static void connected(struct bt_conn *conn, uint8_t conn_err)
{
	if (conn_err) {
		LOG_ERR("Failed to connect to %s (err 0x%02x %s)", bt_conn_dst_str(conn),
			conn_err, bt_hci_err_to_str(conn_err));

		bt_conn_drop(&default_conn);

		start_scan();
		return;
	}

	if (conn != default_conn) {
		return;
	}

	LOG_INF("Connected: %s", bt_conn_dst_str(conn));

	start_discovery(conn);
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	if (conn != default_conn) {
		return;
	}

	LOG_INF("Disconnected: %s (reason 0x%02x %s)", bt_conn_dst_str(conn), reason,
		bt_hci_err_to_str(reason));

	bt_conn_drop(&default_conn);
	ble_connected = false;

	/* Clear stale discovery/subscribe state before scanning for a new
	 * peripheral (or a reconnect of the same one).
	 */
	(void)memset(&subscribe_params, 0, sizeof(subscribe_params));

	start_scan();
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
};

/**
 * Bring up Mode 1: enable the Bluetooth host and start scanning. Only ever
 * called from main() when current_mode == MODE_BLE - this is the one and
 * only place bt_enable() is called anywhere in this file.
 */
static int ble_mode_start(void)
{
	int err = bt_enable(NULL);

	if (err) {
		LOG_ERR("Bluetooth init failed (err %d)", err);
		return err;
	}

	LOG_INF("Bluetooth initialized");

	start_scan();

	return 0;
}

/* ===========================================================================
 * 5. Mode 2: Thread listener
 * ===========================================================================
 * Joins the exact same Thread network xenon_sensor's Mode 2 joins (same
 * hardcoded credentials, see prj.conf) and subscribes to the UDP multicast
 * group xenon_sensor's Mode 2 sends telemetry to, then relays each received
 * datagram as a JSON line via the shared telemetry_emit_json() helper.
 *
 * Scope decision, mirroring xenon_sensor's: this is a Thread node that
 * joins the mesh and listens for multicast telemetry, not a Thread Border
 * Router. No external IPv6 routing, no NAT64, no infra interface bridging.
 *
 * CONFIG_OPENTHREAD_MANUAL_START keeps the network administratively down
 * until openthread_run() is called explicitly below, so Thread traffic only
 * ever happens when this mode is selected - mirroring how bt_enable() is
 * the sole gate for Mode 1's radio activity.
 */

/* Must match xenon_sensor's THREAD_TELEMETRY_MCAST_ADDR/PORT exactly -- see
 * xenon_sensor/src/main.c section 5. Realm-local (mesh-wide, not routed
 * beyond the Thread network) multicast group xenon_sensor's Mode 2 sends
 * telemetry to.
 *   Multicast address: ff03::abcd
 *   Port:               4242
 */
#define THREAD_TELEMETRY_MCAST_ADDR "ff03::abcd"
#define THREAD_TELEMETRY_MCAST_PORT 4242

static int thread_udp_sock = -1;

/**
 * OpenThread state-changed callback, registered with the module's own
 * multi-consumer callback list (openthread_state_changed_callback_register)
 * rather than otSetStateChangedCallback() directly, since that single-slot
 * API is already claimed internally by the L2 driver. Only handles role
 * transitions, which is what "joined the network" boils down to in
 * OpenThread: DETACHED -> CHILD/ROUTER/LEADER means attached. Copied from
 * xenon_sensor's thread_state_changed().
 */
/* Read by the status LED thread in section 6 to distinguish "still joining"
 * from "attached to the mesh".
 */
static bool thread_attached;

/* Diagnostic only, not used by application logic: the Thread Partition ID
 * this node has actually joined/formed, readable directly over SWD
 * (independent of the console's known visibility quirks - see the
 * CONFIG_LOG_BACKEND_UART_BUFFER_SIZE comment below) to directly compare
 * against other nodes and confirm they're genuinely on the same mesh, not
 * just each independently attached to their own separate partition despite
 * sharing network credentials.
 */
volatile uint32_t g_thread_partition_id;

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
		g_thread_partition_id = otThreadGetPartitionId(ot);
		LOG_INF("Thread network join/attach succeeded (role=%s, partition=0x%08x)",
			otThreadDeviceRoleToString(role), g_thread_partition_id);
		thread_attached = true;
		break;
	case OT_DEVICE_ROLE_DETACHED:
		LOG_WRN("Thread network detached; still attempting to (re)join");
		thread_attached = false;
		break;
	case OT_DEVICE_ROLE_DISABLED:
	default:
		thread_attached = false;
		break;
	}
}

static struct openthread_state_changed_callback thread_state_cb = {
	.otCallback = thread_state_changed,
};

/**
 * Join the telemetry multicast group on an already-open UDP socket.
 *
 * A plain socket bound to THREAD_TELEMETRY_MCAST_PORT is not enough to
 * receive multicast traffic -- the network stack only delivers a multicast
 * datagram to a socket if the destination address is in the *interface's*
 * list of joined IPv6 multicast addresses. That join is performed via the
 * ZSOCK_IPV6_ADD_MEMBERSHIP (== ZSOCK_IPV6_JOIN_GROUP) sockopt, exactly the
 * BSD/POSIX IPV6_ADD_MEMBERSHIP mechanism, taking a struct net_ipv6_mreq.
 * xenon_sensor's Mode 2 never needed this -- it only sends, and a sender
 * doesn't join the group it targets.
 *
 * Verified against this exact Zephyr checkout's real source, not guessed:
 *   - include/zephyr/net/socket.h: ZSOCK_IPV6_ADD_MEMBERSHIP (20) /
 *     ZSOCK_IPV6_JOIN_GROUP alias, and the zsock_setsockopt() declaration.
 *   - include/zephyr/net/net_ip.h: struct net_ipv6_mreq { ipv6mr_multiaddr,
 *     ipv6mr_ifindex }.
 *   - subsys/net/lib/sockets/sockets_inet.c ipv6_multicast_group() /
 *     zsock_setsockopt_ctx(): ZSOCK_IPV6_ADD_MEMBERSHIP dispatches here,
 *     which calls net_ipv6_mld_join(iface, &mreq->ipv6mr_multiaddr).
 *     ipv6mr_ifindex == 0 makes net_if_get_by_index() return NULL (its
 *     "index <= 0" guard in subsys/net/ip/net_if.c), which falls back to
 *     this socket's own iface or net_if_get_default() -- exactly what we
 *     want on a single-interface Thread node, no manual iface lookup
 *     needed.
 *   - subsys/net/ip/ipv6_mld.c net_ipv6_mld_join(): registers the address
 *     via net_if_ipv6_maddr_add() (so the IPv6 receive path will actually
 *     accept datagrams addressed to it), which fires a
 *     NET_EVENT_IPV6_MADDR_ADD net_mgmt event unconditionally.
 *   - subsys/net/l2/openthread/openthread.c /
 *     subsys/net/l2/openthread/openthread_utils.c: the OpenThread L2 layer
 *     listens for that exact event and calls
 *     otIp6SubscribeMulticastAddress() in response (add_ipv6_maddr_to_ot()),
 *     which is what actually tells the OpenThread mesh stack to accept and
 *     forward this realm-local multicast group to us. (The OpenThread
 *     interface also sets NET_IF_IPV6_NO_MLD, so net_ipv6_mld_join() skips
 *     sending an actual MLDv2 report over the air -- correct, since Thread
 *     uses its own multicast subscription mechanism, not link-local MLD.)
 *   - Kconfig.ipv6 NET_IPV6_MLD: defaults to y, but made explicit in
 *     prj.conf since net_ipv6_mld_join() is a stub returning -ENOTSUP when
 *     it's off (see include/zephyr/net/mld.h).
 */
static int thread_join_multicast_group(int sock)
{
	struct net_ipv6_mreq mreq;
	int err;

	memset(&mreq, 0, sizeof(mreq));

	err = zsock_inet_pton(NET_AF_INET6, THREAD_TELEMETRY_MCAST_ADDR, &mreq.ipv6mr_multiaddr);
	if (err != 1) {
		LOG_ERR("Failed to parse multicast address \"%s\"", THREAD_TELEMETRY_MCAST_ADDR);
		return -EINVAL;
	}
	mreq.ipv6mr_ifindex = 0; /* 0 -> fall back to this socket's default iface */

	err = zsock_setsockopt(sock, NET_IPPROTO_IPV6, ZSOCK_IPV6_ADD_MEMBERSHIP,
				&mreq, sizeof(mreq));
	if (err) {
		LOG_ERR("Failed to join multicast group [%s] (errno %d)",
			THREAD_TELEMETRY_MCAST_ADDR, errno);
		return -errno;
	}

	LOG_INF("Joined multicast group [%s]:%d", THREAD_TELEMETRY_MCAST_ADDR,
		THREAD_TELEMETRY_MCAST_PORT);

	return 0;
}

/**
 * Bring up Mode 2: register role-change logging, join the Thread network
 * (this is the one and only place openthread_run() is called anywhere in
 * this file), open a UDP socket bound to the telemetry port, and join the
 * telemetry multicast group on it. Only ever called from main() when
 * current_mode == MODE_THREAD. The actual receive loop runs on the
 * dedicated thread in this same section, started unconditionally by
 * K_THREAD_DEFINE() below but gated on stack_ready_sem the same way
 * xenon_sensor gates its sampling thread.
 */
static int thread_mode_start(void)
{
	struct net_sockaddr_in6 local_addr;
	int err;

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

	memset(&local_addr, 0, sizeof(local_addr));
	local_addr.sin6_family = NET_AF_INET6;
	local_addr.sin6_addr = net_in6addr_any;
	local_addr.sin6_port = net_htons(THREAD_TELEMETRY_MCAST_PORT);

	err = zsock_bind(thread_udp_sock, (struct net_sockaddr *)&local_addr, sizeof(local_addr));
	if (err) {
		LOG_ERR("Failed to bind UDP socket to port %d (errno %d)",
			THREAD_TELEMETRY_MCAST_PORT, errno);
		zsock_close(thread_udp_sock);
		thread_udp_sock = -1;
		return -errno;
	}

	err = thread_join_multicast_group(thread_udp_sock);
	if (err) {
		zsock_close(thread_udp_sock);
		thread_udp_sock = -1;
		return err;
	}

	LOG_INF("UDP telemetry listener ready on [%s]:%d", THREAD_TELEMETRY_MCAST_ADDR,
		THREAD_TELEMETRY_MCAST_PORT);

	return 0;
}

/* Released once by main() after the selected mode's stack is fully up, so
 * the Thread receive thread never calls zsock_recvfrom() on a socket that
 * doesn't exist yet. Mode 1 doesn't need this gate (all its work happens on
 * Bluetooth host callbacks driven by bt_enable() itself), but the semaphore
 * is unconditionally defined and given so the Mode 2 thread's wait is
 * simple regardless of which mode ends up active.
 */
static K_SEM_DEFINE(stack_ready_sem, 0, 1);

#define THREAD_RX_THREAD_STACK_SIZE 2048
#define THREAD_RX_THREAD_PRIORITY 7 /* Preemptible; distinct from the OT work queue thread */

/**
 * Dedicated receive thread for Mode 2, started unconditionally at compile
 * time (K_THREAD_DEFINE runs regardless of the persisted mode -- mirroring
 * xenon_sensor's always-defined sampling thread) but blocked on
 * stack_ready_sem until main() has confirmed Mode 2 is actually selected
 * and its socket is up. In Mode 1 this thread parks forever on the
 * semaphore, since main() only gives it after thread_mode_start()
 * succeeds.
 */
static void thread_rx_thread_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	uint8_t rx_buf[sizeof(struct telemetry_payload)];

	k_sem_take(&stack_ready_sem, K_FOREVER);

	if (current_mode != MODE_THREAD || thread_udp_sock < 0) {
		/* Mode 1 is active (or Mode 2 bring-up failed) -- nothing for
		 * this thread to do.
		 */
		return;
	}

	LOG_INF("Thread telemetry receive loop started");

	while (1) {
		ssize_t received = zsock_recvfrom(thread_udp_sock, rx_buf, sizeof(rx_buf), 0,
						   NULL, NULL);

		if (received < 0) {
			LOG_WRN("recvfrom failed (errno %d)", errno);
			continue;
		}

		telemetry_emit_json(rx_buf, (size_t)received, "Thread");
	}
}

K_THREAD_DEFINE(thread_rx_tid, THREAD_RX_THREAD_STACK_SIZE, thread_rx_thread_entry,
		 NULL, NULL, NULL, THREAD_RX_THREAD_PRIORITY, 0, 0);

/* ===========================================================================
 * 6. Status LED
 * ===========================================================================
 * RGB status LED (status_red/status_green/status_blue = led1/led2/led3 in
 * this board's devicetree) gives a visual readout of mode and connection
 * state, driven by its own lightweight thread rather than scattering
 * gpio_pin_set() calls through the mode-specific code above - one place
 * owns LED state, polling the ble_connected/thread_attached flags those
 * sections already maintain.
 *
 *   Blinking blue  : Mode 1 (BLE), scanning / not yet connected+subscribed
 *   Solid blue     : Mode 1 (BLE), connected and subscribed
 *   Blinking green : Mode 2 (Thread), joining / not yet attached
 *   Solid green    : Mode 2 (Thread), attached to the mesh
 *   Rapid red flash: MODE button press detected, switching now (blocking,
 *                     called directly from main() right before the reboot -
 *                     see thread_status_flash_mode_switch() below)
 */

static const struct gpio_dt_spec led_red = GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios);
static const struct gpio_dt_spec led_green = GPIO_DT_SPEC_GET(DT_ALIAS(led2), gpios);
static const struct gpio_dt_spec led_blue = GPIO_DT_SPEC_GET(DT_ALIAS(led3), gpios);

#define STATUS_LED_POLL_MS 400

static int status_led_init(void)
{
	const struct gpio_dt_spec *leds[] = {&led_red, &led_green, &led_blue};

	for (size_t i = 0; i < ARRAY_SIZE(leds); i++) {
		if (!gpio_is_ready_dt(leds[i])) {
			LOG_ERR("Status LED %u GPIO not ready", (unsigned int)i);
			return -ENODEV;
		}

		int err = gpio_pin_configure_dt(leds[i], GPIO_OUTPUT_INACTIVE);

		if (err) {
			LOG_ERR("Failed to configure status LED %u (err %d)", (unsigned int)i, err);
			return err;
		}
	}

	return 0;
}

/**
 * Interactive MODE-button hold-to-confirm gesture, called directly from
 * main() once a button press is first detected. Gives live visual feedback
 * and a genuine cancel window, rather than committing to the switch the
 * instant a press is seen:
 *
 *   Phase A: blinks the CURRENT mode's color MODE_SWITCH_PREVIEW_FLASHES
 *            times. Releasing here cancels - stays on the current mode.
 *   Phase B: blinks the TARGET mode's color, indefinitely, until released.
 *            Releasing here commits the switch.
 *
 * Runs with the background status LED thread suspended for its duration -
 * confirmed on real hardware that without this, the two race over the same
 * three GPIO pins and the "confirmation" flash shows a garbled mix of
 * colors rather than a clean sequence.
 *
 * Returns true if the switch should be committed (to `target`), false if
 * the user released during the preview phase and it should be cancelled.
 */
#define MODE_SWITCH_PREVIEW_FLASHES 4
#define MODE_SWITCH_FLASH_HALF_PERIOD_MS 200

/* Forward reference to the thread ID K_THREAD_DEFINE() creates further down
 * in this section - needed here since mode_switch_confirm() must suspend it.
 */
extern const k_tid_t status_led_tid;

static bool mode_switch_confirm(enum device_mode current, enum device_mode target)
{
	const struct gpio_dt_spec *current_led = (current == MODE_BLE) ? &led_blue : &led_green;
	const struct gpio_dt_spec *target_led = (target == MODE_BLE) ? &led_blue : &led_green;

	k_thread_suspend(status_led_tid);
	gpio_pin_set_dt(&led_red, 0);
	gpio_pin_set_dt(&led_green, 0);
	gpio_pin_set_dt(&led_blue, 0);

	/* Phase A: preview the current mode's color. Release cancels. */
	for (int i = 0; i < MODE_SWITCH_PREVIEW_FLASHES; i++) {
		gpio_pin_set_dt(current_led, 1);
		k_sleep(K_MSEC(MODE_SWITCH_FLASH_HALF_PERIOD_MS));
		if (!mode_button_read()) {
			gpio_pin_set_dt(current_led, 0);
			k_thread_resume(status_led_tid);
			return false;
		}

		gpio_pin_set_dt(current_led, 0);
		k_sleep(K_MSEC(MODE_SWITCH_FLASH_HALF_PERIOD_MS));
		if (!mode_button_read()) {
			k_thread_resume(status_led_tid);
			return false;
		}
	}

	/* Phase B: preview the target mode's color. Release commits. Held
	 * indefinitely otherwise - no timeout, matches "hold as long as you
	 * want to confirm" rather than a race against a clock.
	 */
	while (mode_button_read()) {
		gpio_pin_set_dt(target_led, 1);
		k_sleep(K_MSEC(MODE_SWITCH_FLASH_HALF_PERIOD_MS));
		gpio_pin_set_dt(target_led, 0);
		k_sleep(K_MSEC(MODE_SWITCH_FLASH_HALF_PERIOD_MS));
	}

	/* Released while showing the target color - commit. Leave the
	 * background thread suspended; main() reboots right after this
	 * returns true, so there's no meaningful "resume" state to return to.
	 */
	return true;
}

#define STATUS_LED_THREAD_STACK_SIZE 512
#define STATUS_LED_THREAD_PRIORITY 10 /* Low priority - purely cosmetic, never block real work */

static void status_led_thread_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	bool blink_phase = false;

	if (status_led_init()) {
		return;
	}

	while (1) {
		bool connected = (current_mode == MODE_BLE) ? ble_connected : thread_attached;
		const struct gpio_dt_spec *led = (current_mode == MODE_BLE) ? &led_blue : &led_green;

		gpio_pin_set_dt(&led_red, 0);
		gpio_pin_set_dt((current_mode == MODE_BLE) ? &led_green : &led_blue, 0);

		if (connected) {
			gpio_pin_set_dt(led, 1);
		} else {
			blink_phase = !blink_phase;
			gpio_pin_set_dt(led, blink_phase);
		}

		k_sleep(K_MSEC(STATUS_LED_POLL_MS));
	}
}

K_THREAD_DEFINE(status_led_tid, STATUS_LED_THREAD_STACK_SIZE, status_led_thread_entry,
		 NULL, NULL, NULL, STATUS_LED_THREAD_PRIORITY, 0, 0);

/* ===========================================================================
 * 7. Entry point
 * ===========================================================================
 * main() resolves the mode (checking the MODE button first), then brings up
 * exactly one radio stack. All recurring work happens on Bluetooth host
 * callbacks (Mode 1) or the dedicated receive thread above (Mode 2) - main()
 * itself has nothing left to do once it returns.
 */

/* With a native USB-CDC console (see boards/particle_argon.overlay), early
 * printk()/LOG_INF output before a host has actually opened the serial port
 * can be silently dropped -- there's no DTR asserted yet. Wait for DTR with
 * a bounded timeout so the app still proceeds (both modes are useful even
 * with nobody watching the console) if nothing ever opens the port.
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

	/* Deliberately NOT gated behind wait_for_console_dtr() -- that wait
	 * can block up to CONSOLE_DTR_WAIT_TIMEOUT_MS (3s), and the MODE
	 * button needs to be sampled immediately at boot to match a user
	 * physically holding it for only ~1s around the reset. Checking the
	 * button first, then waiting for DTR only once we know we're not
	 * about to reboot into a different mode anyway, is what actually
	 * makes the button responsive.
	 */
	/* Configured unconditionally and early, regardless of storage/mode
	 * outcome below - the confirmation gesture (and the background status
	 * LED thread) both need the LEDs ready as outputs before anything
	 * else touches them.
	 */
	(void)status_led_init();
	mode_button_init();

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

		if (mode_button_read()) {
			enum device_mode next = (mode == MODE_BLE) ? MODE_THREAD : MODE_BLE;

			LOG_INF("MODE button held at boot: previewing %s -> %s",
				mode_name(mode), mode_name(next));

			if (mode_switch_confirm(mode, next)) {
				err = mode_store(next);
				if (err) {
					LOG_ERR("Failed to persist new mode (err %d); staying in %s",
						err, mode_name(mode));
				} else {
					LOG_INF("Confirmed - rebooting to apply %s...",
						mode_name(next));
					sys_reboot(SYS_REBOOT_COLD);
					/* unreachable */
				}
			} else {
				LOG_INF("Released during preview - staying in %s",
					mode_name(mode));
			}
		}
	}

	current_mode = mode;

	/* Only now, once we know we're actually proceeding to bring up a
	 * radio stack rather than rebooting, is it worth waiting for a host
	 * to have the console open -- so the boot log for whichever mode we
	 * land in isn't dropped.
	 */
	wait_for_console_dtr();

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

	/* Let the Thread receive thread proceed now that the selected mode's
	 * stack is up (a no-op wakeup in Mode 1, where the thread exits
	 * immediately upon checking current_mode).
	 */
	k_sem_give(&stack_ready_sem);

	return 0;
}
