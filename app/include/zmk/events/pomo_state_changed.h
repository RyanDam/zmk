/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * Raised on every pomodoro engine transition: start/pause/resume/cancel
 * and the work→rest / rest→work / session-done rollovers (a14). The
 * notify seam (zmk/notify.h) keeps its visual/beep/light role; this is
 * the machine-readable twin for the Studio `pomo` notification mapper
 * and later h03/h04/d05 consumers.
 */

#pragma once

#include <zephyr/kernel.h>
#include <zmk/event_manager.h>

struct zmk_pomo_state_changed {
    uint8_t state; /* enum zmk_pomo_state                        */
    bool paused;   /* session held; deadline frozen              */
    uint8_t set_idx; /* 1-based index of the running set         */
};

ZMK_EVENT_DECLARE(zmk_pomo_state_changed);
