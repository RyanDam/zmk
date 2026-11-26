/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Coban Tab 1a clock screen config seam.
 * Which layout is live and which style each widget shows is one persistent
 * config: settings "display/clock/config", seeded by the ZMK_DISPLAY_CLOCK_*
 * Kconfig defaults. The Studio RPC `screen` subsystem and the on-glass dev
 * tap-cycle both write it, and both land on one rebuild.
 * Implemented only when CONFIG_ZMK_DISPLAY_CLOCK_SCREEN and
 * CONFIG_ZMK_DISPLAY_DESIGN_SYSTEM are enabled (cobantab1a).
 */

#pragma once

#include <stdint.h>

enum zmk_display_clock_layout {
    ZMK_DISPLAY_CLOCK_LAYOUT_CLOCK_ONLY = 0,    /* big HH:MM, date caption on top */
    ZMK_DISPLAY_CLOCK_LAYOUT_CALENDAR_ONLY = 1, /* year + month header, full grid */
    ZMK_DISPLAY_CLOCK_LAYOUT_FUSION = 2,        /* clock left, calendar right     */
    ZMK_DISPLAY_CLOCK_LAYOUT_COUNT,
};

/** Field values are the stable ids of the widget style enums
 *  (clock_style: zmk_clock_style, calendar_style: zmk_calendar_style) and the
 *  RPC wire values. Never renumber. */
struct zmk_display_clock_config {
    uint8_t layout;         /* enum zmk_display_clock_layout  */
    uint8_t clock_style;    /* enum zmk_clock_style           */
    uint8_t calendar_style; /* enum zmk_calendar_style        */
};

/** Read the live config. Safe from any thread. 0, or -EINVAL. */
int zmk_display_clock_config_get(struct zmk_display_clock_config *out);

/** Apply a config: validate, persist to settings, rebuild the page (posted to
 *  the display work queue — nothing renders here). Safe from any thread.
 *  @return 0 on apply (also when unchanged), -EINVAL on an out-of-range id. */
int zmk_display_clock_config_set(const struct zmk_display_clock_config *cfg);
