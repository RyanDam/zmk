/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Screen-building helpers implementation. The tap math is the a09
 * clock-screen detector moved here so pomodoro and stats reuse it
 * verbatim.
 */

#include <zephyr/sys/util.h>

#include <lvgl.h>

#include <zmk/display.h>
#include <zmk/display/screens.h>

#include "ui_helpers.h"

lv_obj_t *zmk_ui_label(lv_obj_t *parent, enum zmk_display_style_id style_id) {
    lv_obj_t *lbl = lv_label_create(parent);

    lv_obj_add_style(lbl, zmk_display_style(style_id), 0);
    lv_label_set_text(lbl, "");
    return lbl;
}

lv_obj_t *zmk_ui_flex(lv_obj_t *parent, lv_flex_flow_t flow, int gap) {
    lv_obj_t *box = lv_obj_create(parent);

    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(box, flow);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    if (flow == LV_FLEX_FLOW_COLUMN) {
        lv_obj_set_style_pad_row(box, gap, 0);
    } else {
        lv_obj_set_style_pad_column(box, gap, 0);
    }
    return box;
}

#define TAP_MAX_MOVE 24

void zmk_ui_tap_init(struct zmk_ui_tap *tap, const char *page_name, struct k_work *work) {
    tap->page_name = page_name;
    tap->work = work;
}

void zmk_ui_tap_input(struct zmk_ui_tap *tap, struct input_event *evt) {
    switch (evt->type) {
    case INPUT_EV_ABS:
        if (evt->code == INPUT_ABS_X) {
            tap->x = evt->value;
        } else if (evt->code == INPUT_ABS_Y) {
            tap->y = evt->value;
        } else {
            return;
        }
        if (!tap->pressed) {
            tap->start_x = tap->x;
            tap->start_y = tap->y;
        }
        break;

    case INPUT_EV_KEY: {
        int32_t dx, dy;
        int page;

        if (evt->code != INPUT_BTN_TOUCH) {
            return;
        }
        if (evt->value) {
            tap->pressed = true;
            tap->start_x = tap->x;
            tap->start_y = tap->y;
            return;
        }
        if (!tap->pressed) {
            return;
        }
        tap->pressed = false;

        dx = (int32_t)tap->x - (int32_t)tap->start_x;
        dy = (int32_t)tap->y - (int32_t)tap->start_y;
        if (dx < 0) {
            dx = -dx;
        }
        if (dy < 0) {
            dy = -dy;
        }
        if (dx > TAP_MAX_MOVE || dy > TAP_MAX_MOVE) {
            return; /* a swipe: the manager owns it */
        }

        /* The power-on phantom touch lands before the registry exists. */
        page = zmk_display_screen_find(tap->page_name);
        if (page < 0 || zmk_display_screen_current() != page) {
            return;
        }
        k_work_submit_to_queue(zmk_display_work_q(), tap->work);
        break;
    }
    }
}
