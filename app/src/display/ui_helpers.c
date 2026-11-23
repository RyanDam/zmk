/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Screen-building helpers implementation. The tap math is the a09
 * clock-screen detector moved here so pomodoro and stats reuse it
 * verbatim.
 */

#include <string.h>

#include <zephyr/sys/util.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/logging/log.h>

#include <lvgl.h>

#include <zmk/display.h>
#include <zmk/display/screens.h>

#include "ui_helpers.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/* ------------------------------------------------------------------ */
/* LVGL pointer indev (feeds clickable widgets)                        */
/* ------------------------------------------------------------------ */

static atomic_t pt_x;
static atomic_t pt_y;
static atomic_t pt_down;
#if IS_ENABLED(CONFIG_AXS15231B_ROTATION_HW) && ZMK_UI_ROTATION != 0
static atomic_t raw_x; /* last RAW axis pair, for the HW-route remap below */
static atomic_t raw_y;
#endif

static void indev_read_cb(lv_indev_t *indev, lv_indev_data_t *data) {
    ARG_UNUSED(indev);
    data->point.x = (lv_coord_t)atomic_get(&pt_x);
    data->point.y = (lv_coord_t)atomic_get(&pt_y);
    data->state = atomic_get(&pt_down) ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static void ui_input_cb(struct input_event *evt, void *user_data) {
    int32_t value = evt->value;

    ARG_UNUSED(user_data);

#if IS_ENABLED(CONFIG_AXS15231B_ROTATION_HW) && ZMK_UI_ROTATION != 0
    /*
     * Hardware-rotation route: LVGL's display rotation stays 0 (the driver
     * reports swapped caps instead), so its indev correction is a no-op and
     * this feed must supply logical coordinates itself. On the default
     * software route we must NOT do this - lv_indev rotates raw points
     * against lv_display_get_rotation() already.
     */
    int32_t x, y;

    if (evt->type == INPUT_EV_ABS && evt->code == INPUT_ABS_X) {
        atomic_set(&raw_x, value);
    } else if (evt->type == INPUT_EV_ABS && evt->code == INPUT_ABS_Y) {
        atomic_set(&raw_y, value);
    }
    x = atomic_get(&raw_x);
    y = atomic_get(&raw_y);
    zmk_ui_touch_to_logical(&x, &y);
    if (evt->type == INPUT_EV_ABS && evt->code == INPUT_ABS_X) {
        value = x;
    } else if (evt->type == INPUT_EV_ABS && evt->code == INPUT_ABS_Y) {
        value = y;
    }
#endif

    switch (evt->type) {
    case INPUT_EV_ABS:
        if (evt->code == INPUT_ABS_X) {
            atomic_set(&pt_x, value);
        } else if (evt->code == INPUT_ABS_Y) {
            atomic_set(&pt_y, value);
        }
        break;
    case INPUT_EV_KEY:
        if (evt->code == INPUT_BTN_TOUCH) {
            atomic_set(&pt_down, evt->value);
        }
        break;
    default:
        break;
    }
}

INPUT_CALLBACK_DEFINE(NULL, ui_input_cb, NULL);

void zmk_ui_indev_init(void) {
    static bool done;

    if (done) {
        return;
    }
    done = true;

    lv_indev_t *indev = lv_indev_create();

    if (indev == NULL) {
        LOG_ERR("ui: out of memory for LVGL indev");
        return;
    }
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, indev_read_cb);
    LOG_INF("ui: LVGL pointer indev wired to touch input");
}

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
            /* Same press-edge rule as the manager's swipe detector: the
             * driver re-asserts BTN_TOUCH=1 every poll, so latch the start
             * point only on the edge (otherwise a swipe back to its origin
             * still measures dx≈0 and fires as a "tap"). */
            if (!tap->pressed) {
                tap->pressed = true;
                tap->start_x = tap->x;
                tap->start_y = tap->y;
            }
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
        if (tap->page_name == NULL) {
            return; /* tap state not bound (a bug: use compile-time init) */
        }
        page = zmk_display_screen_find(tap->page_name);
        if (page < 0 || zmk_display_screen_current() != page) {
            return;
        }
        k_work_submit_to_queue(zmk_display_work_q(), tap->work);
        break;
    }
    }
}

/* ------------------------------------------------------------------ */
/* Page-bound tickers + display-queue plumbing                         */
/* ------------------------------------------------------------------ */

/* clock/pomodoro/stats 1 Hz ticks + the pomodoro 100 ms breath/fill tick;
 * headroom so a12/a13 landers do not silently starve (warn below). */
#define PAGE_TIMERS_MAX 6

struct page_timer {
    const char *page_name;
    zmk_ui_page_tick_cb cb;
};

static struct page_timer page_timers[PAGE_TIMERS_MAX];

static void page_timer_cb(lv_timer_t *timer) {
    struct page_timer *pt = lv_timer_get_user_data(timer);
    int page;

    /* Legacy status-screen mode: no registry, always tick. */
    page = zmk_display_screen_find(pt->page_name);
    if (page >= 0 && zmk_display_screen_current() != page) {
        return;
    }
    pt->cb();
}

lv_timer_t *zmk_ui_page_timer(const char *page_name, uint32_t period_ms, zmk_ui_page_tick_cb cb) {
    for (int i = 0; i < PAGE_TIMERS_MAX; i++) {
        if (page_timers[i].page_name == NULL) {
            page_timers[i].page_name = page_name;
            page_timers[i].cb = cb;
            return lv_timer_create(page_timer_cb, period_ms, &page_timers[i]);
        }
    }
    LOG_WRN("ui: no free page-timer slot for '%s' - page will not refresh", page_name);
    return NULL;
}

void zmk_ui_post(struct k_work *work) { k_work_submit_to_queue(zmk_display_work_q(), work); }
