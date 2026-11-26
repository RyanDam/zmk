/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Clock widget implementation (see clock_widget.h).
 * Colors/fonts only through zmk_display_style(), and every tick update is
 * skip-if-unchanged: this panel is full-refresh, so a needless invalidation
 * costs a ~61 ms flush.
 */

#include <string.h>

#include <zephyr/kernel.h>

#include <lvgl.h>

#include "clock_widget.h"
#include "styles.h"
#include "ui_helpers.h"

/* [size][style] -> registry style: HUGE is the 144 px hero, STACKED the 72 px
 * hour-over-minute fusion clock, LARGE the 48 px side clock. */
static const enum zmk_display_style_id
    clock_style_map[ZMK_CLOCK_SIZE_COUNT][ZMK_CLOCK_STYLE_COUNT] = {
        [ZMK_CLOCK_SIZE_HUGE] = {ZMK_STYLE_TEXT_HERO_XL},
        [ZMK_CLOCK_SIZE_LARGE] = {ZMK_STYLE_TEXT_SUBHERO},
        [ZMK_CLOCK_SIZE_STACKED] = {ZMK_STYLE_TEXT_HERO},
};

struct zmk_clock_widget {
    lv_obj_t *obj;       /* what the container aligns: label or box      */
    lv_obj_t *labels[2]; /* single face: [0] only; stacked: hour, minute */
    uint8_t size;
    uint8_t style;
    uint8_t applied;  /* registry id currently added to the labels     */
    char cache[2][4]; /* last-drawn text: unchanged -> no invalidation  */
};

static int label_count(zmk_clock_widget_t *w) { return w->size == ZMK_CLOCK_SIZE_STACKED ? 2 : 1; }

static void clock_widget_apply(zmk_clock_widget_t *w) {
    enum zmk_display_style_id want = clock_style_map[w->size][w->style];

    if (w->applied == want) {
        return;
    }
    if (w->applied < ZMK_STYLE_COUNT) {
        for (int i = 0; i < label_count(w); i++) {
            lv_obj_remove_style(w->labels[i], zmk_display_style(w->applied), 0);
        }
    }
    for (int i = 0; i < label_count(w); i++) {
        lv_obj_add_style(w->labels[i], zmk_display_style(want), 0);
    }
    w->applied = want;
}

/* Freed with the subtree on the container's lv_obj_clean() */
static void clock_widget_delete_cb(lv_event_t *e) { lv_free(lv_event_get_user_data(e)); }

zmk_clock_widget_t *zmk_clock_widget_create(lv_obj_t *parent, enum zmk_clock_size size,
                                            enum zmk_clock_style style) {
    zmk_clock_widget_t *w;

    if (parent == NULL || size >= ZMK_CLOCK_SIZE_COUNT || style >= ZMK_CLOCK_STYLE_COUNT) {
        return NULL;
    }

    w = lv_malloc(sizeof(*w));
    if (w == NULL) {
        return NULL;
    }
    memset(w, 0, sizeof(*w));
    w->size = size;
    w->style = style;
    w->applied = ZMK_STYLE_COUNT; /* nothing added yet */

    if (size == ZMK_CLOCK_SIZE_STACKED) {
        /* Hour above minute: at 72 px the line height alone glued the digits
         * together, so give the rows a grid-unit gap. */
        w->obj = zmk_ui_flex(parent, LV_FLEX_FLOW_COLUMN, ZMK_GRID_UNIT * 2);
        w->labels[0] = lv_label_create(w->obj);
        w->labels[1] = lv_label_create(w->obj);
        lv_label_set_text(w->labels[0], "--");
        lv_label_set_text(w->labels[1], "--");
    } else {
        w->obj = lv_label_create(parent);
        w->labels[0] = w->obj;
        lv_label_set_text(w->labels[0], "--:--");
    }
    clock_widget_apply(w);
    lv_obj_add_event_cb(w->obj, clock_widget_delete_cb, LV_EVENT_DELETE, w);

    return w;
}

lv_obj_t *zmk_clock_widget_obj(zmk_clock_widget_t *w) { return w == NULL ? NULL : w->obj; }

int zmk_clock_widget_set_style(zmk_clock_widget_t *w, enum zmk_clock_style style) {
    if (w == NULL || style >= ZMK_CLOCK_STYLE_COUNT) {
        return -EINVAL;
    }
    w->style = style;
    clock_widget_apply(w);
    return 0;
}

enum zmk_clock_style zmk_clock_widget_get_style(const zmk_clock_widget_t *w) {
    return w == NULL ? ZMK_CLOCK_STYLE_SANS : (enum zmk_clock_style)w->style;
}

void zmk_clock_widget_update(zmk_clock_widget_t *w, const struct zmk_time *t) {
    char buf[8];

    if (w == NULL || t == NULL) {
        return;
    }

    if (w->size == ZMK_CLOCK_SIZE_STACKED) {
        snprintk(buf, sizeof(buf), "%02u", t->hour);
        zmk_ui_set_text(w->cache[0], sizeof(w->cache[0]), w->labels[0], buf);
        snprintk(buf, sizeof(buf), "%02u", t->minute);
        zmk_ui_set_text(w->cache[1], sizeof(w->cache[1]), w->labels[1], buf);
        return;
    }

    snprintk(buf, sizeof(buf), "%02u:%02u", t->hour, t->minute);
    zmk_ui_set_text(w->cache[0], sizeof(w->cache[0]), w->labels[0], buf);
}
