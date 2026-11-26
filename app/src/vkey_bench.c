/*
 * Forensics bench (CONFIG_ZMK_VKEY_BENCH): firmware-resident
 * position-edge hammer with NO console at all — if the wedge persists
 * without the USB-Serial-JTAG console in the loop, it is pure BLE TX.
 *
 * Liveness = watch the Mac. Position 9, 80 ms hold, every 500 ms.
 * Bench only; never product.
 */

#include <zephyr/kernel.h>

#include <zmk/events/position_state_changed.h>
#include <zmk/behavior.h>

#include <zmk/endpoints.h>

#define BENCH_POS 9
#define BENCH_HOLD_MS 80
#define BENCH_PERIOD_MS 500

static struct k_work_delayable press_dw;
static struct k_work_delayable release_dw;

static void fire(struct zmk_position_state_changed *ev) {
	int err = raise_zmk_position_state_changed(*ev);

	(void)err; /* no LOG here — that is the entire point */
}

static void press_cb(struct k_work *work) {
	struct zmk_position_state_changed ev = {
		.source = ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL,
		.position = BENCH_POS,
		.state = true,
		.timestamp = k_uptime_get(),
	};

	fire(&ev);
	k_work_schedule(&release_dw, K_MSEC(BENCH_HOLD_MS));
}

static void release_cb(struct k_work *work) {
	struct zmk_position_state_changed ev = {
		.source = ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL,
		.position = BENCH_POS,
		.state = false,
		.timestamp = k_uptime_get(),
	};

	fire(&ev);
	k_work_schedule(&press_dw, K_MSEC(BENCH_PERIOD_MS));
}

static int vkey_bench_init(void) {
	k_work_init_delayable(&press_dw, press_cb);
	k_work_init_delayable(&release_dw, release_cb);
	/* let BT come up and the Mac reconnect before the first edge */
	k_work_schedule(&press_dw, K_SECONDS(12));
	return 0;
}

SYS_INIT(vkey_bench_init, APPLICATION, 90);
