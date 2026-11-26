/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * Raised when the design-system theme changes from anywhere — Studio
 * RPC set, the preset cycler, settings load (a07 theme.c choke point).
 * Colors are packed 0xRRGGBB so the event header stays LVGL-free;
 * the Studio `theme` mapper (a14) republishes them to the host.
 */

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
