/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Coban Tab 1a pomodoro session engine. State/kind ids mirror pad10a
 * screen_pomodoro.h:19-40 — the Studio RPC `pomo` proto uses the same
 * numbers. All calls are safe from any thread (spinlock-guarded).
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

/** Session state. */
enum zmk_pomo_state {
    ZMK_POMO_STATE_IDLE = 0x00,
    ZMK_POMO_STATE_WORK = 0x01,
    ZMK_POMO_STATE_REST = 0x02,
};

/** Notification mode. */
enum zmk_pomo_noti_mode {
    ZMK_POMO_NOTI_NONE = 0x00,
    ZMK_POMO_NOTI_BEEP = 0x01,
    ZMK_POMO_NOTI_LIGHT = 0x02,
    ZMK_POMO_NOTI_BOTH = 0x03,
};

/** Persisted session config; all limits enforced by zmk_pomo_config_set. */
struct zmk_pomo_config {
    uint8_t sets;      /* work+rest sets per session, 1-8 */
    uint16_t work_s;   /* seconds, 30-3600 */
    uint16_t rest_s;   /* seconds, 10-3600 */
    uint8_t noti_mode; /* enum zmk_pomo_noti_mode */
};

/** UI snapshot, moment-in-time copy. */
struct zmk_pomo_status {
    uint8_t state;          /* enum zmk_pomo_state */
    bool paused;            /* deadline frozen */
    uint8_t set_idx;        /* 1-based */
    uint8_t sets;
    uint16_t phase_total_s;
    int64_t deadline_ms;    /* k_uptime ms when the phase ends */
    int64_t phase_start_ms;
    int64_t pause_at_ms;
};

/** 0, -EBUSY if running. Takes the display blank lock until the session
 *  ends. */
int zmk_pomo_start(void);

/** 0, -EINVAL when idle (pause) / when not paused (resume). */
int zmk_pomo_pause(void);
int zmk_pomo_resume(void);

/** Abort the session, releasing the blank lock. */
int zmk_pomo_cancel(void);

bool zmk_pomo_running(void);

void zmk_pomo_get_status(struct zmk_pomo_status *out);

/** Remaining ms of the running phase (0 when idle). */
int64_t zmk_pomo_remaining_ms(void);

/** Pointer stays valid (copy to mutate). */
const struct zmk_pomo_config *zmk_pomo_config_get(void);

/** Clamp + persist + apply (live for the next phase of a running session). */
void zmk_pomo_config_set(const struct zmk_pomo_config *cfg);
