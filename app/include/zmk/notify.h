/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Coban Tab 1a notify seam (backlog a10).
 *
 * Features announce events ("rest starts", "session done") on channels,
 * and whichever backends are registered answer. v1 ships ONE backend —
 * visual (screen wake + banner, src/display/notify_visual.c). The beep
 * and light channels are real in the API today so the h03 (NS4168 I2S)
 * and h04 (WS2812 light bar) items only have to register a function; a
 * notification on a channel with no backend just logs.
 *
 * pad10a had no seam: screen_pomodoro called audio_click / RGB effects
 * directly. The channel enum keeps those two futures addressable.
 */

#pragma once

/** Output channels a backend can serve. */
enum zmk_notify_channel {
    ZMK_NOTIFY_CH_BEEP,  /* h03 (NS4168 I2S tones) will register   */
    ZMK_NOTIFY_CH_LIGHT, /* h04 (WS2812 light bar) will register   */
    ZMK_NOTIFY_CH_COUNT,
};

/** What happened. Names state the event, not the phase it starts. */
enum zmk_notify_kind {
    ZMK_NOTIFY_POMO_WORK_DONE,    /* a work phase ended, rest begins   */
    ZMK_NOTIFY_POMO_REST_DONE,    /* a rest phase ended, work begins   */
    ZMK_NOTIFY_POMO_SESSION_DONE, /* all sets finished                 */
    ZMK_NOTIFY_POMO_CANCELLED,    /* user cancelled the session        */
    ZMK_NOTIFY_KIND_COUNT,
};

/** Backend entry point. Called from whatever thread fired the notify —
 *  implementations must post their own work if they touch LVGL/hardware. */
typedef void (*zmk_notify_backend_fn)(enum zmk_notify_kind kind, enum zmk_notify_channel channel);

/** Register a backend for one channel (one per channel; replace allowed). */
int zmk_notify_register_backend(enum zmk_notify_channel channel, zmk_notify_backend_fn backend);

/** Fire @p kind on @p channel; no backend = debug log only. */
void zmk_notify(enum zmk_notify_channel channel, enum zmk_notify_kind kind);

/** Stable lowercase name for logs/banners (never NULL). */
const char *zmk_notify_kind_name(enum zmk_notify_kind kind);
