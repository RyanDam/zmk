/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * Raised when the screen manager's visible page changes (swipe, dots,
 * Studio RPC navigation, boot home restore). Subscribers: the Studio
 * `screen` notification mapper (a14), debug overlays (d05). Name is a
 * copy of the registered screen id (zmk/display/screens.h).
 */

#pragma once

#include <zephyr/kernel.h>
#include <zmk/event_manager.h>

#define ZMK_DISPLAY_SCREEN_NAME_MAX 16

struct zmk_display_screen_changed {
    int32_t index;
    char name[ZMK_DISPLAY_SCREEN_NAME_MAX];
};

ZMK_EVENT_DECLARE(zmk_display_screen_changed);
