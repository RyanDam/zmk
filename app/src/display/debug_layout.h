/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Layout debug borders (CONFIG_ZMK_DISPLAY_DEBUG_LAYOUT): outline every
 * plain LVGL container of a screen — the ones already built and any created
 * later — so page boxes and padding are readable on the glass. Compiled out
 * unless the flag is on.
 */

#pragma once

#include <zephyr/sys/util.h>

#include <lvgl.h>

#if IS_ENABLED(CONFIG_ZMK_DISPLAY_DEBUG_LAYOUT)

void zmk_display_debug_layout_watch(lv_obj_t *screen);

#else

static inline void zmk_display_debug_layout_watch(lv_obj_t *screen) { ARG_UNUSED(screen); }

#endif
