/*
 * argon_gateway - BLE central "gateway" firmware for the Particle Argon
 * (Nordic nRF52840).
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Role in the two-board demo
 * ---------------------------------------------------------------------
 * This board is the BLE GATT *central*. It never advertises and never
 * accepts inbound connections (CONFIG_BT_PERIPHERAL is deliberately left
 * off) -- it only scans, connects out, discovers, and subscribes. The
 * counterpart board (xenon_sensor) is the GATT *peripheral*: it samples a
 * sensor on a dedicated RTOS thread and notifies readings over BLE. This
 * app's BLE work all happens on Zephyr's Bluetooth host callbacks/system
 * workqueue, so there's no hand-rolled thread here (unlike xenon_sensor's
 * explicit sampling thread) -- that's a deliberate concurrency choice, not
 * an oversight.
 *
 * The Argon's onboard ESP32 Wi-Fi co-processor (attached via &uart1 in the
 * board's devicetree) is entirely out of scope for this app: BLE only.
 *
 * Wire format design note
 * ---------------------------------------------------------------------
 * Two different serialization formats are used on purpose, one per link:
 *
 *   - BLE notification payload: a tight 8-byte packed little-endian binary
 *     struct (struct telemetry_payload below). The radio link is bandwidth-
 *     and power-constrained, so every byte on the air matters.
 *
 *   - USB serial (this board -> host PC): a JSON line
 *     ({"temp_c": 23.50, "vdd_mv": 3300, "seq": 12}\n). The serial link to a
 *     development host is comparatively unconstrained, and JSON is trivial
 *     for tools/telemetry_monitor.py (a plain Python script) to parse
 *     without any custom binary decoding on that side. Re-encoding at this
 *     boundary is the right tradeoff in both directions.
 *
 * USB-CDC / console note
 * ---------------------------------------------------------------------
 * particle_argon's board defconfig (particle_argon_defconfig) already turns
 * on CONFIG_SERIAL, CONFIG_CONSOLE, and CONFIG_UART_CONSOLE unconditionally,
 * and the upstream board devicetree (particle_argon.dts) does not define a
 * USB CDC-ACM node or a "zephyr,console" chosen override. That means
 * enabling CONFIG_USB_DEVICE_STACK/CONFIG_USB_CDC_ACM from this app's
 * prj.conf alone would be dead configuration (Kconfig can't fabricate the
 * devicetree node a CDC-ACM console needs), and doing it properly would
 * require a devicetree overlay outside this app's scope. So printk() below
 * rides the board's already-enabled standard console UART path rather than
 * duplicating/fighting the board defaults -- see prj.conf for the same note.
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>
#include <zephyr/logging/log.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/att.h>

LOG_MODULE_REGISTER(argon_gateway, LOG_LEVEL_INF);

/* ===========================================================================
 * Protocol spec (shared byte-for-byte with the xenon_sensor peripheral)
 * ===========================================================================
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

/* Exact wire layout of a single BLE notification payload. Must stay
 * byte-for-byte identical to the peripheral firmware's definition.
 */
struct __packed telemetry_payload {
	int16_t temp_centi_c;  /* hundredths of a degree C */
	uint16_t vdd_mv;        /* millivolts */
	uint32_t seq;            /* monotonic sample counter */
};

BUILD_ASSERT(sizeof(struct telemetry_payload) == 8,
	     "telemetry_payload must match the 8-byte BLE wire format");

/* ===========================================================================
 * BLE central state
 * ===========================================================================
 */

static struct bt_conn *default_conn;

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

/* ===========================================================================
 * Notification handling / serial output
 *
 * Decodes each BLE notification's packed binary payload and re-emits it as
 * a JSON line on the USB serial console. Kept separate from the
 * scan/connect/discover logic below.
 * ===========================================================================
 */

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

	if (length != sizeof(struct telemetry_payload)) {
		LOG_WRN("Dropping notification with unexpected length %u (expected %u)",
			length, (unsigned int)sizeof(struct telemetry_payload));
		return BT_GATT_ITER_CONTINUE;
	}

	/* Decode the packed little-endian struct field-by-field rather than
	 * trusting host struct layout/endianness to match the wire exactly.
	 * The nRF52840 is little-endian, so this is a no-op here in practice,
	 * but being explicit keeps the decode correct if this code is ever
	 * reused on a big-endian host.
	 */
	struct telemetry_payload payload;

	memcpy(&payload, data, sizeof(payload));

	int16_t temp_centi_c = (int16_t)sys_le16_to_cpu((uint16_t)payload.temp_centi_c);
	uint16_t vdd_mv = sys_le16_to_cpu(payload.vdd_mv);
	uint32_t seq = sys_le32_to_cpu(payload.seq);

	float temp_c = (float)temp_centi_c / 100.0f;

	/* JSON on USB serial for tools/telemetry_monitor.py -- see the file
	 * header comment for why this differs from the BLE wire format.
	 */
	printk("{\"temp_c\": %.2f, \"vdd_mv\": %u, \"seq\": %u}\n",
	       (double)temp_c, vdd_mv, seq);

	LOG_INF("Telemetry notification: temp=%.2fC vdd=%umV seq=%u",
		(double)temp_c, vdd_mv, seq);

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
 * decision, per the task spec's guidance that UUID filtering is the more
 * robust of the two.
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

/* ===========================================================================
 * Entry point
 * ===========================================================================
 */

int main(void)
{
	int err = bt_enable(NULL);

	if (err) {
		LOG_ERR("Bluetooth init failed (err %d)", err);
		return 0;
	}

	LOG_INF("Bluetooth initialized");

	start_scan();

	return 0;
}
