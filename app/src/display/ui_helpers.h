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

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include <lvgl.h>

#include <zephyr/devicetree.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>

#include "styles.h"

/* ------------------------------------------------------------------ */
/* Product orientation (mirrors the panel node's `rotation`)           */
/* ------------------------------------------------------------------ */

/*
 * The touch driver always reports RAW panel coordinates (0..319 x 0..479 on
 * the portrait glass) regardless of the product rotation. LVGL corrects its
 * own widgets itself (lv_indev runs pointer reads through
 * lv_display_rotate_point), but the raw ZMK input listeners (screen swipes,
 * band/hit-test taps) must convert before comparing against layout, or a
 * vertical swipe would navigate pages sideways.
 */
#if DT_HAS_CHOSEN(zephyr_display)
#define ZMK_UI_PANEL_W DT_PROP_OR(DT_CHOSEN(zephyr_display), width, 0)
#define ZMK_UI_PANEL_H DT_PROP_OR(DT_CHOSEN(zephyr_display), height, 0)
#define ZMK_UI_ROTATION DT_PROP_OR(DT_CHOSEN(zephyr_display), rotation, 0)
#else
#define ZMK_UI_PANEL_W 0
#define ZMK_UI_PANEL_H 0
#define ZMK_UI_ROTATION 0
#endif

/** Map a raw touch point into the logical (laid-out) coordinate space,
 *  with the exact same mapping LVGL applies to pointer input. */
static inline void zmk_ui_touch_to_logical(int32_t *x, int32_t *y) {
    const int32_t rx = *x, ry = *y;

    switch (ZMK_UI_ROTATION) {
    case 90:
        *x = ZMK_UI_PANEL_H - 1 - ry;
        *y = rx;
        break;
    case 180:
        *x = ZMK_UI_PANEL_W - 1 - rx;
        *y = ZMK_UI_PANEL_H - 1 - ry;
        break;
    case 270:
        *x = ry;
        *y = ZMK_UI_PANEL_W - 1 - rx;
        break;
    default:
        break;
    }
}

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

/**
 * @brief Wire touch input into LVGL as a pointer indev.
 *
 * Without this, clickable LVGL objects (a10's pomodoro buttons, the a13
 * key tiles) never receive PRESSED/CLICKED events — the screen-manager
 * swipe handler and the tap detectors read raw input events, and nothing
 * fed lv_indev. Called once by the screen manager on the display work
 * queue, after LVGL init. Raw consumers (swipes, tap bands) keep seeing
 * the same events independently, so the >= 64 px swipe contract still
 * holds: a swipe that ends where it started on a clickable still reads
 * as a click, everything else is PRESS_LOST.
 */
void zmk_ui_indev_init(void);

/* ------------------------------------------------------------------ */
/* Review-pass helpers (2026-11-21): tick gating, text caching, work   */
/* boilerplate, style toggling                                         */
/* ------------------------------------------------------------------ */

/**
 * @brief Set a label only when the text actually changed.
 *
 * On this full-refresh panel, any invalidation costs a ~61 ms flush, so
 * per-tick updates must not poke labels that already show the string.
 * The caller owns @p cache (per-label, same size as the formatted
 * buffer); the first call with a given string does write.
 */
static inline void zmk_ui_set_text(char *cache, size_t cache_len, lv_obj_t *label,
                                   const char *text) {
    if (strncmp(cache, text, cache_len) == 0) {
        return;
    }
    strncpy(cache, text, cache_len - 1);
    cache[cache_len - 1] = '\0';
    lv_label_set_text(label, text);
}

/** Add or remove a registry style as one intent. */
static inline void zmk_ui_style_toggle(lv_obj_t *obj, const lv_style_t *style, bool on) {
    if (on) {
        lv_obj_add_style(obj, style, 0);
    } else {
        lv_obj_remove_style(obj, style, 0);
    }
}

/**
 * @brief 1 Hz-style updater bound to a page: ticks only while that page
 * is current (pages are separate LVGL screens, so hidden pages
 * shouldn't even compute their strings). In legacy status-screen mode
 * (no screen manager) the timer always runs.
 */
typedef void (*zmk_ui_page_tick_cb)(void);
lv_timer_t *zmk_ui_page_timer(const char *page_name, uint32_t period_ms, zmk_ui_page_tick_cb cb);

/**
 * @brief Define a work item posted to the display work queue.
 *
 *   ZMK_UI_WORK_DEFINE(my_work, my_cb) { ... }        (function body)
 *   zmk_ui_post(&my_work);                            (any thread)
 */
#define ZMK_UI_WORK_DEFINE(work_sym, fn_name)                                                      \
    static void fn_name(struct k_work *work);                                                      \
    K_WORK_DEFINE(work_sym, fn_name);                                                              \
    static void fn_name(struct k_work *work)

void zmk_ui_post(struct k_work *work);
