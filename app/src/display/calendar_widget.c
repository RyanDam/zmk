/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Calendar widget implementation (see calendar_widget.h).
 *
 * Wave-1 owner pass: header year + month both on the 28 px title face,
 * square day cells on an even lattice (col_gap == row_gap -> equal pitch
 * both ways), adjacent-month days kept at 50 % text, today marked by a
 * white 25 % pip larger than the cell, day digits on barlow 24.
 *
 * All rows (header, weekday, 6 weeks) are children of ONE column flex
 * with row_gap padding, so the vertical pitch is exactly cell + row_gap
 * everywhere; the whole block rebuilds on a date change (once a day),
 * which keeps header and grid in lockstep with no half-updated states.
 *
 * a07 rule: colors/fonts/opacity classes only through zmk_display_style();
 * cell sizes and gaps are layout, set by the container through the opts.
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <lvgl.h>

#include "calendar_widget.h"
#include "styles.h"
#include "ui_helpers.h"

static const char *const month_full[] = {"January",   "February", "March",    "April",
                                         "May",       "June",     "July",     "August",
                                         "September", "October",  "November", "December"};

struct zmk_calendar_widget {
    lv_obj_t *root; /* column flex: header row, weekday row, 6 weeks */
    uint8_t style;
    bool header;
    uint16_t header_w; /* 0 = the 7-cell grid width                   */
    uint16_t cell;
    uint16_t col_gap;
    uint16_t row_gap;
    uint16_t grid_w;
    uint16_t band_h;
    int32_t date_key; /* yyyymmdd drawn in the grid, 0 = none          */
    bool have_last;   /* last holds the newest reading                 */
    struct zmk_time last;
};

static int days_in_month(uint8_t month, uint16_t year) {
    static const uint8_t lengths[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int days = lengths[month - 1];

    if (month == 2 && (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0))) {
        days++;
    }
    return days;
}

/* Line height of a registry style's font — needed to stretch the row
 * gaps without hardcoding font metrics. */
static int style_line_h(enum zmk_display_style_id id) {
    lv_style_value_t v;

    if (lv_style_get_prop(zmk_display_style(id), LV_STYLE_TEXT_FONT, &v) !=
        LV_STYLE_RES_NOT_FOUND) {
        return lv_font_get_line_height((const lv_font_t *)v.ptr);
    }
    return 24;
}

/* One square cell: a centering box; on today the digit label lives INSIDE
 * the white pip (a sibling would be a second flex item and sit NEXT to
 * the disc — the owner photo bug), so it always paints on top of it.
 * Out-of-month digits use the 50 % text style. */
static void add_day_cell(zmk_calendar_widget_t *w, lv_obj_t *row, int shown, bool dim, bool today) {
    lv_obj_t *cell = zmk_ui_flex(row, LV_FLEX_FLOW_ROW, 0);
    lv_obj_t *host = cell;

    lv_obj_set_size(cell, w->cell, w->cell);

    if (today) {
        lv_obj_t *pip = lv_obj_create(cell);

        lv_obj_remove_style_all(pip);
        /* Slightly wider than the cell: a confident disc that still
         * leaves row_gap - 2 px of dark between neighbours. */
        lv_obj_set_size(pip, w->cell + 2, w->cell + 2);
        lv_obj_set_style_pad_all(pip, 0, 0);
        lv_obj_add_style(pip, zmk_display_style(ZMK_STYLE_TODAY), 0);
        lv_obj_center(pip);
        host = pip;
    }

    lv_obj_t *lbl = zmk_ui_label(host, dim ? ZMK_STYLE_TEXT_DAY_DIM : ZMK_STYLE_TEXT_DAY);

    if (host != cell) {
        lv_obj_center(lbl); /* inside the plain pip; the flex cell centres itself */
    }
    lv_label_set_text_fmt(lbl, "%d", shown);
}

/* Weekday/week row: cells packed col_gap apart, or spread SPACE_BETWEEN
 * across grid_w so the lattice fills the container (full-page view). */
static lv_obj_t *new_row(zmk_calendar_widget_t *w) {
    lv_obj_t *row = zmk_ui_flex(w->root, LV_FLEX_FLOW_ROW, w->col_gap);

    if (w->grid_w != 0) {
        lv_obj_set_width(row, w->grid_w);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
    }
    return row;
}

static void build_block(zmk_calendar_widget_t *w, const struct zmk_time *t) {
    static const char headers[] = "SMTWTFS";
    /* Weekday of the 1st, derived from today's (0 = Sunday). */
    int first_wd = (t->weekday + 7 - ((t->day - 1) % 7)) % 7;
    int dim = days_in_month(t->month, t->year);
    int prev_month = t->month == 1 ? 12 : t->month - 1;
    int prev_dim = days_in_month(prev_month, t->month == 1 ? t->year - 1 : t->year);
    /* Only as many week rows as the month actually occupies (4..6) —
     * a sixth all-grey week after a 5-week month read as broken dates
     * running on (owner photo pass). With band_h set, the slack of a
     * short month is spread into the flex row gaps so the block still
     * fills the page instead of hanging above a gap. */
    int rows = (first_wd + dim + 6) / 7;
    int n_children = rows + 1 + (w->header ? 1 : 0);
    int natural =
        style_line_h(ZMK_STYLE_TEXT_SECONDARY) + rows * w->cell + (n_children - 1) * w->row_gap;
    int pad = w->row_gap;

    if (w->header) {
        natural += style_line_h(ZMK_STYLE_TEXT_TITLE);
    }
    if (w->band_h > natural) {
        pad += (w->band_h - natural) / (n_children - 1);
    }

    lv_obj_clean(w->root);
    lv_obj_set_style_pad_row(w->root, pad, 0);

    if (w->header) {
        /* Year left, month name right — same 28 px face, the year kept
         * one shade back by the secondary title style. Spread across
         * the page (full view) or the grid (fusion). */
        lv_obj_t *row = zmk_ui_flex(w->root, LV_FLEX_FLOW_ROW, 0);

        lv_obj_set_width(row, w->header_w);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);

        lv_obj_t *year = zmk_ui_label(row, ZMK_STYLE_TEXT_TITLE_SEC);
        lv_label_set_text_fmt(year, "%04u", t->year);

        lv_obj_t *month = zmk_ui_label(row, ZMK_STYLE_TEXT_TITLE);
        lv_label_set_text(month, month_full[t->month - 1]);
    }

    /* Weekday letters, one per column. */
    lv_obj_t *hrow = new_row(w);

    for (int c = 0; c < 7; c++) {
        lv_obj_t *lbl = zmk_ui_label(hrow, ZMK_STYLE_TEXT_SECONDARY);
        char ch[2] = {headers[c], '\0'};

        lv_obj_set_width(lbl, w->cell);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(lbl, ch);
    }

    /* The occupied weeks; days <=0 and >dim are the neighbouring
     * months, shown dim so the grid stays a full rectangle.
     * (Named adj, NOT dim: a second `dim` here shadowed the days-in-
     * month int and printed raw 32..41 — the photo-proof bug.) */
    for (int r = 0; r < rows; r++) {
        lv_obj_t *wrow = new_row(w);

        for (int c = 0; c < 7; c++) {
            int day = r * 7 + c - first_wd + 1;
            bool adj = day < 1 || day > dim;
            int shown = day < 1 ? prev_dim + day : (day > dim ? day - dim : day);

            add_day_cell(w, wrow, shown, adj, !adj && day == t->day);
        }
    }
}

static void calendar_widget_render(zmk_calendar_widget_t *w) {
    if (!w->have_last) {
        return;
    }

    int32_t key = (int32_t)w->last.year * 10000 + w->last.month * 100 + w->last.day;

    if (key != w->date_key) {
        w->date_key = key;
        build_block(w, &w->last);
    }
}

/* Freed together with the widget subtree on the container's rebuild. */
static void calendar_widget_delete_cb(lv_event_t *e) { lv_free(lv_event_get_user_data(e)); }

zmk_calendar_widget_t *zmk_calendar_widget_create(lv_obj_t *parent,
                                                  const struct zmk_calendar_opts *opts) {
    zmk_calendar_widget_t *w;

    if (parent == NULL || opts == NULL || opts->style >= ZMK_CALENDAR_STYLE_COUNT ||
        opts->cell < 26 || opts->col_gap > 64 || opts->row_gap > 64) {
        return NULL;
    }

    w = lv_malloc(sizeof(*w));
    if (w == NULL) {
        return NULL;
    }
    memset(w, 0, sizeof(*w));
    w->style = opts->style;
    w->header = opts->header;
    w->header_w = opts->header_w != 0 ? opts->header_w : (7 * opts->cell + 6 * opts->col_gap);
    w->cell = opts->cell;
    w->col_gap = opts->col_gap;
    w->row_gap = opts->row_gap;
    w->grid_w = opts->grid_w;
    w->band_h = opts->band_h;

    w->root = zmk_ui_flex(parent, LV_FLEX_FLOW_COLUMN, opts->row_gap);

    lv_obj_add_event_cb(w->root, calendar_widget_delete_cb, LV_EVENT_DELETE, w);

    return w;
}

lv_obj_t *zmk_calendar_widget_obj(zmk_calendar_widget_t *w) { return w == NULL ? NULL : w->root; }

int zmk_calendar_widget_set_style(zmk_calendar_widget_t *w, enum zmk_calendar_style style) {
    if (w == NULL || style >= ZMK_CALENDAR_STYLE_COUNT) {
        return -EINVAL;
    }
    if ((enum zmk_calendar_style)w->style == style) {
        return 0;
    }
    w->style = style;
    w->date_key = 0; /* re-render the grid in the new style */
    calendar_widget_render(w);
    return 0;
}

enum zmk_calendar_style zmk_calendar_widget_get_style(const zmk_calendar_widget_t *w) {
    return w == NULL ? ZMK_CALENDAR_STYLE_GRID : (enum zmk_calendar_style)w->style;
}

void zmk_calendar_widget_update(zmk_calendar_widget_t *w, const struct zmk_time *t) {
    if (w == NULL || t == NULL) {
        return;
    }

    w->last = *t;
    w->have_last = true;
    calendar_widget_render(w);
}
