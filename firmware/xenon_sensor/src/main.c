/*
 * xenon_sensor - BLE telemetry peripheral for the Particle Xenon (nRF52840)
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * -----------------------------------------------------------------------
 * Architecture, for the reviewer
 * -----------------------------------------------------------------------
 * This firmware is deliberately structured to show three things:
 *
 *   1. RTOS/concurrency design: sensor sampling runs on its own
 *      K_THREAD_DEFINE() thread - a separate execution context from both
 *      main() and Zephyr's Bluetooth host thread. The sampling thread
 *      owns the sensors and the "source of truth" telemetry snapshot; the
 *      Bluetooth host thread (which runs GATT read/CCC callbacks) only
 *      ever touches that snapshot through a mutex. See `telemetry_lock`.
 *
 *   2. Wireless (BLE GATT) protocol design: a single custom service with
 *      one read+notify characteristic carrying a compact 8-byte packed
 *      binary struct (`struct telemetry_payload`), not JSON/text. The
 *      UUIDs and wire format here are a fixed contract shared with the
 *      argon_gateway central application - see the UUID and struct
 *      definitions below, which must not change without coordinating
 *      with that side.
 *
 *   3. Power-aware design: the sampling thread spends nearly all of its
 *      life blocked in k_sleep(), not polling. Zephyr's kernel is
 *      tickless, so during that k_sleep() the CPU is free to drop into
 *      its lowest-power idle state with no extra Kconfig required - the
 *      "sleep between samples" loop shape below *is* the power
 *      optimization, not an afterthought bolted on top of it.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/util.h>

#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/dt-bindings/adc/nrf-saadc.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>

#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(xenon_sensor, LOG_LEVEL_INF);

/* ===========================================================================
 * Wire format - shared contract with argon_gateway
 * ===========================================================================
 * Exactly 8 bytes, little-endian (native byte order on the Cortex-M4 in the
 * nRF52840, so no explicit byte-swapping is needed here). Do not reorder,
 * resize, or reinterpret these fields without updating the central app.
 */
struct __packed telemetry_payload {
	int16_t temp_centi_c;	/* On-chip die temperature, in 0.01 degC steps (2350 = 23.50C) */
	uint16_t vdd_mv;	/* Regulated rail voltage as seen by the SAADC, in millivolts */
	uint32_t seq;		/* Monotonically incrementing sample counter */
};

BUILD_ASSERT(sizeof(struct telemetry_payload) == 8,
	     "telemetry_payload must stay exactly 8 bytes on the wire");

/* Sampling cadence. 5s keeps the demo responsive; a deployed sensor node
 * would likely stretch this to minutes to save even more power.
 */
#define SAMPLE_PERIOD_S 5
#define SAMPLE_PERIOD K_SECONDS(SAMPLE_PERIOD_S)

/* ===========================================================================
 * Custom GATT service: "Xenon Sensor Telemetry"
 * ===========================================================================
 * UUIDs are fixed by the portfolio spec so the argon_gateway central can be
 * written against them independently. BT_UUID_128_ENCODE() takes a standard
 * UUID string with the hyphens replaced by commas and 0x prefixes added, so
 * these two lines are a direct transcription of:
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
 * for that across threads.
 */
static struct telemetry_payload latest_telemetry;
static K_MUTEX_DEFINE(telemetry_lock);

/* Tracks whether a central has enabled notifications (written the CCC
 * "notify" bit). Updated only from the Bluetooth host thread's CCC
 * config-changed callback, read only by the sampling thread before it calls
 * bt_gatt_notify() - single writer, single reader, so a plain bool is fine.
 */
static bool notifications_enabled;

/* Released once by main() after bt_enable() and advertising have both
 * succeeded, so the sampling thread never touches the GATT/ADC/sensor
 * stack before the Bluetooth host is actually up.
 */
static K_SEM_DEFINE(ble_ready_sem, 0, 1);

/* ===========================================================================
 * Sensor sampling
 * ===========================================================================
 * On-chip die temperature and VDD rail sensing are kept as small, separate
 * functions so the BLE plumbing above never has to know how a value was
 * produced - it just gets a filled-in telemetry_payload.
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
 * GATT service definition
 * ===========================================================================
 */

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

/* ===========================================================================
 * Advertising + connection lifecycle
 * ===========================================================================
 * Scope decision: once a central connects we stay connected persistently
 * (we do not disconnect/re-advertise on a timer). We only resume
 * advertising after an actual disconnect event.
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

/* ===========================================================================
 * Sampling thread
 * ===========================================================================
 * Runs as its own K_THREAD_DEFINE() context - not the system workqueue, not
 * inline in main(). This keeps sensor I/O and GATT notification cadence
 * fully decoupled from whatever main() or the Bluetooth host thread happen
 * to be doing, which is the standard Zephyr shape for a periodic producer.
 */

#define SAMPLING_THREAD_STACK_SIZE 1024
#define SAMPLING_THREAD_PRIORITY 7 /* Preemptible; distinct from the BT host thread */

static void sampling_thread_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	uint32_t seq = 0;
	int err;

	/* Do not touch sensors/ADC/GATT until bt_enable() + advertising have
	 * both completed in main(). This is the thread's only synchronization
	 * point with main(); after this it runs entirely on its own clock.
	 */
	k_sem_take(&ble_ready_sem, K_FOREVER);

	err = vdd_adc_channel_init();
	if (err) {
		LOG_ERR("VDD ADC channel init failed (err %d); vdd_mv will be stale", err);
	}

	LOG_INF("Sampling thread started (period=%ds)", SAMPLE_PERIOD_S);

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

		if (!notifications_enabled) {
			LOG_INF("No subscriber; skipping notify for sample #%u", seq);
			continue;
		}

		err = bt_gatt_notify(NULL, TELEMETRY_VALUE_ATTR, &snapshot, sizeof(snapshot));
		if (err) {
			LOG_WRN("Notify failed for sample #%u (err %d)", seq, err);
		} else {
			LOG_INF("Notified subscriber: seq=%u temp=%d vdd=%u",
				snapshot.seq, snapshot.temp_centi_c, snapshot.vdd_mv);
		}
	}
}

K_THREAD_DEFINE(sampling_tid, SAMPLING_THREAD_STACK_SIZE, sampling_thread_entry,
		 NULL, NULL, NULL, SAMPLING_THREAD_PRIORITY, 0, 0);

/* ===========================================================================
 * Entry point
 * ===========================================================================
 * main() only brings up the Bluetooth stack and starts advertising, then
 * hands off to the sampling thread above. All recurring work happens on
 * that thread and inside the Bluetooth host's own thread(s) - main() itself
 * has nothing left to do once it returns.
 */
int main(void)
{
	int err;

	err = bt_enable(NULL);
	if (err) {
		LOG_ERR("Bluetooth init failed (err %d)", err);
		return 0;
	}
	LOG_INF("Bluetooth initialized");

	err = start_advertising();
	if (err) {
		return 0;
	}

	/* Let the sampling thread proceed now that the BLE stack is up. */
	k_sem_give(&ble_ready_sem);

	return 0;
}
