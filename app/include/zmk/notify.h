/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Coban Tab 1a notify seam: backends register per channel; a notify on a
 * channel with no backend just logs.
 */

#pragma once

/** Output channels a backend can serve. */
enum zmk_notify_channel {
    ZMK_NOTIFY_CH_BEEP,  /* NS4168 I2S tones */
    ZMK_NOTIFY_CH_LIGHT, /* WS2812 light bar */
    ZMK_NOTIFY_CH_COUNT,
};

/** What happened. */
enum zmk_notify_kind {
    ZMK_NOTIFY_POMO_WORK_DONE,
    ZMK_NOTIFY_POMO_REST_DONE,
    ZMK_NOTIFY_POMO_SESSION_DONE,
    ZMK_NOTIFY_POMO_CANCELLED,
    ZMK_NOTIFY_KIND_COUNT,
};

/** Backend entry point: runs on the firing thread — post work for LVGL/hardware. */
typedef void (*zmk_notify_backend_fn)(enum zmk_notify_kind kind, enum zmk_notify_channel channel);

/** Register a backend for one channel (one per channel; replace allowed). */
int zmk_notify_register_backend(enum zmk_notify_channel channel, zmk_notify_backend_fn backend);

/** Fire @p kind on @p channel; no backend = debug log only. */
void zmk_notify(enum zmk_notify_channel channel, enum zmk_notify_kind kind);

/** Stable lowercase name for logs/banners (never NULL). */
const char *zmk_notify_kind_name(enum zmk_notify_kind kind);
