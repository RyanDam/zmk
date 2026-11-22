/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Calendar widget module for the Coban Tab 1a screens (wave-1 clock
 * audit).
 *
 * One module owns the month view: an optional header row (year left,
 * month name right, both the 28 px title face) plus the weekday header
 * and exactly as many week rows as the month occupies (4..6 — never an
 * all-grey extra week). Day cells are SQUARE (`cell` px): the grid reads
 * as an even lattice on the 480x320 panel.
 * Adjacent-month days render with the 50 % text style, and today sits
 * on a whisper-white pip that is slightly LARGER than the cell.
 *
 * The style enum is the seam the a14 Studio RPC drives — ids are stable,
 * a future view (week agenda, list) adds an id and no screen code
 * changes.
 *
 * The whole block (header + grid) rebuilds when the civil date changes
 * (roughly once a day) or the style is swapped — cheap, and it keeps
 * header and grid in lockstep with no half-updated states.
 *
 * Call set_style/update from the display work queue only;
 * zmk_display_clock_config_set() is the any-thread entry point.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <lvgl.h>

#include <zmk/display/time_src.h>

enum zmk_calendar_style {
    ZMK_CALENDAR_STYLE_GRID = 0, /* weekday header + 4..6 week rows */
    ZMK_CALENDAR_STYLE_COUNT,
};

struct zmk_calendar_opts {
    enum zmk_calendar_style style;
    bool header;       /* year on the left, month name on the right    */
    uint16_t header_w; /* header row width (0 = the 7-cell grid width) */
    uint16_t cell;     /* SQUARE day cell side, px (>= 26: barlow 24)  */
    uint16_t col_gap;  /* between week-row cells (when grid_w == 0)    */
    uint16_t row_gap;  /* between header/weekday/week rows             */
    uint16_t grid_w;   /* >0: rows stretch to this width, cells spread
                        * SPACE_BETWEEN across it (fill the page)      */
    uint16_t band_h;   /* >0: row gaps grow so the block — only as many
                        * week rows as the month occupies (4..6) —
                        * fills this height                            */
};

typedef struct zmk_calendar_widget zmk_calendar_widget_t;

/** Build the widget inside @p parent (the container positions it).
 *  Returns NULL on a bad arg or LVGL OOM. */
zmk_calendar_widget_t *zmk_calendar_widget_create(lv_obj_t *parent,
                                                  const struct zmk_calendar_opts *opts);

/** The widget's LVGL object, for the container to align. */
lv_obj_t *zmk_calendar_widget_obj(zmk_calendar_widget_t *w);

/** Swap the view style live (display work queue). 0, or -EINVAL. */
int zmk_calendar_widget_set_style(zmk_calendar_widget_t *w, enum zmk_calendar_style style);

enum zmk_calendar_style zmk_calendar_widget_get_style(const zmk_calendar_widget_t *w);

/** Update header labels + move the today pip on a date change. NULL-safe;
 *  call from the page tick. */
void zmk_calendar_widget_update(zmk_calendar_widget_t *w, const struct zmk_time *t);
