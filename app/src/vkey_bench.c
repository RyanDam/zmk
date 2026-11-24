/*
 * a13 forensics bench (CONFIG_ZMK_VKEY_BENCH): firmware-resident
 * position-edge hammer with NO console at all.
 *
 * Every scripted repro so far fired taps through the shell over the
 * ESP32-S3's own USB-Serial-JTAG console — so "died at tap ~5-10 with a
 * live BLE link" always co-packed BLE notification TX with USJ console
 * traffic on the same chip. This build compiles SHELL and LOG out and
 * hammers the keymap itself: if the wedge persists, it is pure BLE TX;
 * if it survives, the wedge needs the console in the loop (and the
 * product fix — production has no console anyway — sidesteps it).
 *
 * Liveness = watch the Mac: keystrokes keep arriving / device stays
 * "Connected" in Bluetooth settings. Position 9, 80 ms hold, every 500 ms
 * (same shape as /tmp/tab1a-repro-v2.py). Bench only; never product.
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
