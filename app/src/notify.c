/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Notify seam implementation. Dispatch only — no policy, no hardware;
 * a notify on a channel without a backend logs and drops.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zmk/notify.h>

LOG_MODULE_REGISTER(zmk_notify, CONFIG_ZMK_LOG_LEVEL);

static zmk_notify_backend_fn backends[ZMK_NOTIFY_CH_COUNT];

static const char *const kind_names[] = {
    [ZMK_NOTIFY_POMO_WORK_DONE] = "pomo-work-done",
    [ZMK_NOTIFY_POMO_REST_DONE] = "pomo-rest-done",
    [ZMK_NOTIFY_POMO_SESSION_DONE] = "pomo-session-done",
    [ZMK_NOTIFY_POMO_CANCELLED] = "pomo-cancelled",
};

static const char *const channel_names[] = {
    [ZMK_NOTIFY_CH_BEEP] = "beep",
    [ZMK_NOTIFY_CH_LIGHT] = "light",
};

int zmk_notify_register_backend(enum zmk_notify_channel channel, zmk_notify_backend_fn backend) {
    if (channel >= ZMK_NOTIFY_CH_COUNT || backend == NULL) {
        return -EINVAL;
    }

    backends[channel] = backend;
    LOG_INF("notify backend registered on channel %s", channel_names[channel]);
    return 0;
}

void zmk_notify(enum zmk_notify_channel channel, enum zmk_notify_kind kind) {
    if (channel >= ZMK_NOTIFY_CH_COUNT || kind >= ZMK_NOTIFY_KIND_COUNT) {
        return;
    }

    if (backends[channel] != NULL) {
        backends[channel](kind, channel);
    } else {
        LOG_DBG("notify %s on %s: no backend yet", zmk_notify_kind_name(kind),
                channel_names[channel]);
    }
}

const char *zmk_notify_kind_name(enum zmk_notify_kind kind) {
    if (kind >= ZMK_NOTIFY_KIND_COUNT || kind_names[kind] == NULL) {
        return "unknown";
    }
    return kind_names[kind];
}
