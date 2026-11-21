/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Coban Tab 1a pomodoro session engine (backlog a10).
 *
 * Port of pad10a's pomodoro (graphics/screens/screen_pomodoro.[ch]) split
 * into engine (here) and screen (src/display/pomodoro_screen.c): pad10a
 * kept the timing inside the LVGL screen callback, so a hidden screen and
 * a running session were impossible. The engine runs on Zephyr timers with
 * absolute-deadline arithmetic (drift-free by construction — the a10
 * "< 1 s per 25 min" box follows), and only announces through the notify
 * seam (zmk/notify.h).
 *
 * The state/kind ids mirror pad10a's screen_pomodoro.h:19-40 so the a14
 * Studio RPC can port its `pomo` proto onto the same numbers.
 *
 * All calls are safe from any thread; internal state is a spinlock-guarded
 * snapshot read by the UI at screen-refresh rate.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

/** Session state. idle = no session; work/rest = running phases. */
enum zmk_pomo_state {
    ZMK_POMO_STATE_IDLE = 0x00,
    ZMK_POMO_STATE_WORK = 0x01,
    ZMK_POMO_STATE_REST = 0x02,
};

/** Notification mode, ids ported from pad10a screen_pomodoro.h:19-24. */
enum zmk_pomo_noti_mode {
    ZMK_POMO_NOTI_NONE = 0x00,
    ZMK_POMO_NOTI_BEEP = 0x01,  /* h03 gives this a backend    */
    ZMK_POMO_NOTI_LIGHT = 0x02, /* h04 gives this a backend   */
    ZMK_POMO_NOTI_BOTH = 0x03,
};

/** Persisted session config (pad10a eeprom cb_eeprom.h:36-40 field set). */
struct zmk_pomo_config {
    uint8_t sets;      /* work+rest sets per session, 1-8           */
    uint16_t work_s;   /* work phase length, seconds, 30-3600       */
    uint16_t rest_s;   /* rest phase length, seconds, 10-3600       */
    uint8_t noti_mode; /* enum zmk_pomo_noti_mode                  */
};

/** UI snapshot; every field is a moment-in-time copy. */
struct zmk_pomo_status {
    uint8_t state;          /* enum zmk_pomo_state                        */
    bool paused;            /* session held; deadline frozen              */
    uint8_t set_idx;        /* 1-based index of the running set           */
    uint8_t sets;           /* session config copy for pip drawing        */
    uint16_t phase_total_s; /* running phase length, for progress  */
    int64_t deadline_ms;    /* k_uptime ms when the phase ends     */
    int64_t phase_start_ms; /* k_uptime ms when the phase began    */
    int64_t pause_at_ms;    /* k_uptime ms of the pause (while held)*/
};

/** Start a session with the persisted config. Returns 0, -EBUSY if
 * running. Takes the display blank lock until the session ends. */
int zmk_pomo_start(void);

/** Hold the session (deadline freezes). 0, -EINVAL when idle. */
int zmk_pomo_pause(void);

/** Resume after pause. 0, -EINVAL when not paused. */
int zmk_pomo_resume(void);

/** Abort the session, releasing the blank lock. */
int zmk_pomo_cancel(void);

bool zmk_pomo_running(void);

/** Copy the live status. */
void zmk_pomo_get_status(struct zmk_pomo_status *out);

/** Remaining ms of the running phase (0 when idle). */
int64_t zmk_pomo_remaining_ms(void);

/** Read the persisted config (pointer stays valid, copy to mutate). */
const struct zmk_pomo_config *zmk_pomo_config_get(void);

/** Clamp + persist + apply (live for the next phase of a running
 * session). Called by the screen's dev-cycle and, later, a14's RPC. */
void zmk_pomo_config_set(const struct zmk_pomo_config *cfg);
