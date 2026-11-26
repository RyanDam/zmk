/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

/* Raised on theme change; colors are packed 0xRRGGBB so this header stays LVGL-free. */

#pragma once

#include <zephyr/kernel.h>
#include <zmk/event_manager.h>

struct zmk_display_theme_changed {
    uint32_t background;
    uint32_t text_primary;
    uint32_t text_secondary;
    uint32_t accent;
    uint32_t inactive;
};

ZMK_EVENT_DECLARE(zmk_display_theme_changed);
