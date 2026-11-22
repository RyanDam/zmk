/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Clock widget module for the Coban Tab 1a screens (wave-1 clock audit).
 *
 * One module owns the "HH:MM" readout: the screen decides WHERE and HOW
 * BIG (container picks a size class from the a07 ramp), the widget style
 * picks the face. Time text is fixed HH:MM 24 h — the a09 pad10a format
 * matrix (12/24 h, AM/PM, date-format enum) was cut in the wave-1 audit
 * as unnecessary surface.
 *
 * The style enum is the seam the a14 Studio RPC drives: ids are stable
 * (never renumber), a new face adds an id + registry styles and no
 * screen code changes. Call set_style/update from the display work
 * queue only (LVGL is not thread-safe) — zmk_display_clock_config_set()
 * in <zmk/display/clock_screen.h> is the any-thread entry point.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <lvgl.h>

#include <zmk/display/time_src.h>

enum zmk_clock_style {
    ZMK_CLOCK_STYLE_SANS = 0, /* Bai Jamjuree (144 / 48 px slots)  */
    ZMK_CLOCK_STYLE_COUNT,
};

/* The a09 serif (48 px DM Serif) and tabular (20 px Barlow) faces render
 * too small for the hero clock — the owner cut them in the wave-1 audit.
 * They return as new ids when the font ramp grows same-size slots for
 * those faces; until then the widget has exactly one style and the big
 * clock is the only clock. */

/* Size classes the container asks for; each style maps them onto the
 * best available slot of the registry ramp (see clock_widget.c). */
enum zmk_clock_size {
    ZMK_CLOCK_SIZE_HUGE = 0,    /* clock-only hero: HH:MM 144 px      */
    ZMK_CLOCK_SIZE_LARGE = 1,   /* side clock: HH:MM 48 px            */
    ZMK_CLOCK_SIZE_STACKED = 2, /* fusion: hour over minute, 72 px    */
    ZMK_CLOCK_SIZE_COUNT,
};

typedef struct zmk_clock_widget zmk_clock_widget_t;

/** Build the widget inside @p parent (the container positions it).
 *  Returns NULL on a bad arg or LVGL OOM. */
zmk_clock_widget_t *zmk_clock_widget_create(lv_obj_t *parent, enum zmk_clock_size size,
                                            enum zmk_clock_style style);

/** The widget's LVGL object, for the container to align. */
lv_obj_t *zmk_clock_widget_obj(zmk_clock_widget_t *w);

/** Swap the face live (display work queue). 0, or -EINVAL. */
int zmk_clock_widget_set_style(zmk_clock_widget_t *w, enum zmk_clock_style style);

enum zmk_clock_style zmk_clock_widget_get_style(const zmk_clock_widget_t *w);

/** Redraw HH:MM (skip-if-unchanged). NULL-safe; call from the page tick. */
void zmk_clock_widget_update(zmk_clock_widget_t *w, const struct zmk_time *t);
