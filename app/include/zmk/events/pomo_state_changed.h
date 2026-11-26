/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

/* Raised on every pomodoro transition; machine-readable twin of the notify seam. */

#pragma once

#include <zephyr/kernel.h>
#include <zmk/event_manager.h>

struct zmk_pomo_state_changed {
    uint8_t state; /* enum zmk_pomo_state                        */
    bool paused;   /* session held; deadline frozen              */
    uint8_t set_idx; /* 1-based index of the running set         */
};

ZMK_EVENT_DECLARE(zmk_pomo_state_changed);
