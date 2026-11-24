/*
 * a13 bench central — connects to the Coban Tab (open link, no SMP),
 * subscribes to the first HID Report characteristic with notify, logs
 * every notification and the exact disconnect reason + time-to-death.
 *
 * Purpose: phone/Mac-free repro of the S3 BLE wedge. If the Tab dies
 * against this dumb central too, the bug is device-side, period.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>

LOG_MODULE_REGISTER(c3central, LOG_LEVEL_INF);

static struct bt_conn *conn;
static int64_t connected_at;
static int64_t last_notify_ms;
static uint32_t notify_count;
static bool subscribed;
static bool connect_pending;

static void start_scan(void);

/* ---------- notification sink ---------- */

static uint8_t notify_cb(struct bt_conn *c,
			 struct bt_gatt_subscribe_params *params,
			 const void *data, uint16_t length)
{
	if (data == NULL) {
		LOG_WRN("SUB LOST (notifies %u)", notify_count);
		subscribed = false;
		return BT_GATT_ITER_STOP;
	}

	notify_count++;
	last_notify_ms = k_uptime_get();
	if ((notify_count % 10U) == 1U) {
		const uint8_t *rpt = data;

		LOG_INF("NOTIFY #%u len %u b1 0x%02x", notify_count, length,
			(length > 1U) ? rpt[1] : 0U);
	}

	return BT_GATT_ITER_CONTINUE;
}

static void sub_cb_write(struct bt_conn *c, uint8_t err,
			 struct bt_gatt_subscribe_params *params)
{
	LOG_INF("CCC write err %u", err);
}

static struct bt_gatt_subscribe_params sub_params;

/* ---------- discovery ---------- */

static uint8_t disc_cb(struct bt_conn *c, const struct bt_gatt_attr *attr,
		       struct bt_gatt_discover_params *params)
{
	if (attr == NULL) {
		if (!subscribed) {
			LOG_ERR("DISCOVER done, no report char found");
		}
		return BT_GATT_ITER_STOP;
	}

	if (attr->user_data != NULL) {
		struct bt_gatt_chrc *ch = attr->user_data;

		if ((bt_uuid_cmp(ch->uuid, BT_UUID_HIDS_REPORT) == 0) &&
		    ((ch->properties & BT_GATT_CHRC_NOTIFY) != 0U)) {
			/* ZMK server layout: CCC sits right after the value */
			sub_params.value_handle = ch->value_handle;
			sub_params.ccc_handle = ch->value_handle + 1U;
			sub_params.value = BT_GATT_CCC_NOTIFY;
			sub_params.notify = notify_cb;
			sub_params.subscribe = sub_cb_write;

			int err = bt_gatt_subscribe(c, &sub_params);

			if (err != 0) {
				LOG_ERR("SUBSCRIBE failed %d", err);
			} else {
				subscribed = true;
				LOG_INF("SUBSCRIBED vh %u ccc %u",
					sub_params.value_handle,
					sub_params.ccc_handle);
			}
			return BT_GATT_ITER_STOP;
		}
	}

	return BT_GATT_ITER_CONTINUE;
}

static void discover_report(struct bt_conn *c)
{
	static struct bt_gatt_discover_params disc_params = {
		.type = BT_GATT_DISCOVER_CHARACTERISTIC,
		.func = disc_cb,
		.start_handle = 0x0001U,
		.end_handle = 0xffffU,
	};

	int err = bt_gatt_discover(c, &disc_params);

	if (err != 0) {
		LOG_ERR("DISCOVER start failed %d", err);
	}
}

/* ---------- connection lifecycle ---------- */

static void on_connected(struct bt_conn *c, uint8_t err)
{
	connect_pending = false;

	if (err != 0U) {
		LOG_WRN("CONNECT failed err %u", err);
		start_scan();
		return;
	}

	connected_at = k_uptime_get();
	notify_count = 0U;
	subscribed = false;
	conn = c;

	LOG_INF("CONNECTED (open link, no pairing) t0");
	discover_report(c);
}

static void on_disconnected(struct bt_conn *c, uint8_t reason)
{
	int64_t life = k_uptime_get() - connected_at;

	LOG_WRN("DISCONNECTED reason %u after %lld ms, notifies %u, "
		"last notify %lld ms before death",
		reason, life, notify_count,
		last_notify_ms - k_uptime_get());

	if (conn != NULL) {
		bt_conn_unref(conn);
		conn = NULL;
	}
	subscribed = false;
	start_scan();
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = on_connected,
	.disconnected = on_disconnected,
};

/* ---------- scanning ---------- */

static bool name_cb(struct bt_data *data, void *user_data)
{
	bool *found = user_data;

	if ((data->type == BT_DATA_NAME_COMPLETE) ||
	    (data->type == BT_DATA_NAME_SHORTENED)) {
		if ((data->data_len >= 3U) &&
		    (strncmp(data->data, "Tab", 3) == 0)) {
			*found = true;
			return false;
		}
	}

	return true;
}

static void device_found(const bt_addr_le_t *addr, int8_t rssi, uint8_t type,
			 struct net_buf_simple *ad)
{
	bool found = false;

	if (connect_pending || (conn != NULL)) {
		return;
	}

	(void)bt_data_parse(ad, name_cb, &found);
	if (!found) {
		return;
	}

	LOG_INF("FOUND %s rssi %d - connecting OPEN", bt_addr_le_str(addr),
		rssi);
	connect_pending = true;

	if (bt_le_scan_stop() != 0) {
		LOG_WRN("scan stop failed");
	}

	int err = bt_conn_le_create(addr, BT_CONN_LE_CREATE_CONN,
				    BT_LE_CONN_PARAM_DEFAULT, &conn);

	if (err != 0) {
		LOG_ERR("CONN_CREATE failed %d", err);
		connect_pending = false;
		start_scan();
	}
}

static struct bt_le_scan_param scan_param = {
	.type = BT_LE_SCAN_TYPE_PASSIVE,
	.options = BT_LE_SCAN_OPT_NONE,
	.interval = 0x0140U,
	.window = 0x0060U,
};

static void start_scan(void)
{
	int err = bt_le_scan_start(&scan_param, device_found);

	if ((err != 0) && (err != -EALREADY)) {
		LOG_ERR("SCAN start failed %d", err);
	} else {
		LOG_INF("scanning for Tab...");
	}
}

/* ---------- heartbeat ---------- */

static void hb_fn(struct k_work *work);
K_WORK_DELAYABLE_DEFINE(hb_work, hb_fn);

static void hb_fn(struct k_work *work)
{
	LOG_INF("HEARTBEAT notifies %u age %lld s %s", notify_count,
		(conn != NULL) ? (k_uptime_get() - last_notify_ms) / 1000 : -1,
		(conn != NULL) ? ((subscribed) ? "LINK-UP" : "link-no-sub")
			       : "no-link");
	k_work_reschedule(&hb_work.work, K_SECONDS(5));
}

int main(void)
{
	LOG_INF("c3-central up: looking for a Tab (open link, no SMP)");

	int err = bt_enable(NULL);

	if (err != 0) {
		LOG_ERR("bt enable %d", err);
		return 0;
	}

	k_work_reschedule(&hb_work.work, K_SECONDS(5));
	start_scan();

	return 0;
}
