/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Screen-building helpers shared by the tab1a pages (a09 clock, a10
 * pomodoro, a11 stats). Only layout plumbing — styling still goes
 * through zmk_display_style() exclusively (the a07 rule).
 */

#pragma once

#include <lvgl.h>

#include <zephyr/input/input.h>
#include <zephyr/kernel.h>

#include "styles.h"

/** Label carrying exactly one registry style. */
lv_obj_t *zmk_ui_label(lv_obj_t *parent, enum zmk_display_style_id style_id);

/** Transparent content-sized flex box (COLUMN or ROW) with `gap` between
 * children. Touched only on rebuild, never per tick. */
lv_obj_t *zmk_ui_flex(lv_obj_t *parent, lv_flex_flow_t flow, int gap);

/**
 * @brief Tap detector for a specific page.
 *
 * A tap is a press-and-release that moved less than 24 px on both axes,
 * so the screen manager's swipe navigation (>= 64 px horizontal) is never
 * stolen. When the page named @p page_name is current, the tap posts
 * @p work to the display work queue.
 *
 * Usage in a screen file:
 *   static ZMK_UI_TAP_STATE(my_tap);
 *   INPUT_CALLBACK_DEFINE(NULL, my_cb, NULL) {
 *       zmk_ui_tap_input(&my_tap, evt);
 *   }
 * with my_tap.page_name / .work filled in once (see zmk_ui_tap_init).
 */
struct zmk_ui_tap {
    bool pressed;
    uint16_t x, y;
    uint16_t start_x, start_y;
    const char *page_name;
    struct k_work *work;
};

#define ZMK_UI_TAP_STATE(name) static struct zmk_ui_tap name

void zmk_ui_tap_init(struct zmk_ui_tap *tap, const char *page_name, struct k_work *work);

/** Feed one input event; call from an INPUT_CALLBACK_DEFINE. */
void zmk_ui_tap_input(struct zmk_ui_tap *tap, struct input_event *evt);
