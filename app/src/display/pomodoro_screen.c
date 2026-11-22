/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Pomodoro screen for the Coban Tab 1a (backlog a10).
 *
 * pad10a's screen_pomodoro.c owned both the timer and the UI; here the
 * UI only renders zmk_pomo_get_status() at 1 Hz and fires engine calls
 * from the touch buttons, so a session runs with any page on screen and
 * a14's RPC can drive the same engine.
 *
 * Layout on the a07 grid: title top, progress ring (LV_PART_MAIN track
 * + LV_PART_INDICATOR progress, a07 registry styles) with MM:SS inside,
 * set pips, phase word, and — in the thumb-weighted reach band — the
 * Start / Pause / Cancel buttons (>= 44 px targets). Below them a
 * settings strip: tapping that band (not a button) dev-cycles
 * sets -> work -> rest -> notification mode, pad10a-style config made
 * reachable without a host.
 *
 * The a08 blank lock is the engine's job, not this screen's.
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/input/input.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <lvgl.h>

#include <zmk/display.h>
#include <zmk/display/screens.h>
#include <zmk/pomodoro.h>

#include "styles.h"
#include "ui_helpers.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define PAGE_NAME "pomodoro"

#define RING_SIZE 160 /* 20 grid units: fits the 320-tall canvas beside the
                       * control column (was 208 for the 480-tall portrait) */

/* Dev-cycle choices. Short entries first so a bench session can watch a
 * full session complete (d06); the 1-minute run also proves the engine's
 * phase rollover without a 25-minute sit. */
static const uint8_t sets_choices[] = {1, 2, 4, 6};
static const uint16_t work_choices[] = {60, 300, 900, 1500};
static const uint16_t rest_choices[] = {60, 300, 600};
static const uint8_t noti_choices[] = {ZMK_POMO_NOTI_NONE, ZMK_POMO_NOTI_BEEP, ZMK_POMO_NOTI_LIGHT,
                                       ZMK_POMO_NOTI_BOTH};

struct pomo_ui {
    lv_obj_t *root;
    lv_obj_t *ring;
    lv_obj_t *time_label;
    lv_obj_t *phase_label;
    lv_obj_t *pips;
    lv_obj_t *pip_objs[8];
    uint8_t pip_count;
    lv_obj_t *btn_start;
    lv_obj_t *btn_pause;
    lv_obj_t *btn_cancel;
    lv_obj_t *btn_label_start;
    lv_obj_t *btn_label_pause;
    lv_obj_t *cfg_label;
    /* last-drawn state: a tick that changes nothing invalidates nothing
     * (each invalidation costs a ~61 ms full-frame flush) */
    char time_cache[8];
    char phase_cache[8];
    char cfg_cache[48];
    char pause_cache[8];
    int32_t arc_cache;
    int pips_done_shown;
    bool idle_shown;
    bool paused_shown;
};

static struct pomo_ui ui;
static uint8_t cfg_dev_field; /* which settings field the strip tap moves */

/* ------------------------------------------------------------------ */
/* Buttons                                                             */
/* ------------------------------------------------------------------ */

static void button_clicked_cb(lv_event_t *e) {
    uintptr_t action = (uintptr_t)lv_event_get_user_data(e);

    switch (action) {
    case 0: /* Start: visible only when idle */
        zmk_pomo_start();
        break;
    case 1: /* Pause <-> Resume toggle */
        if (zmk_pomo_pause() != 0) {
            zmk_pomo_resume();
        }
        break;
    case 2:
        zmk_pomo_cancel();
        break;
    }
}

static lv_obj_t *mk_button(lv_obj_t *parent, const char *text, uintptr_t action,
                           lv_obj_t **label_out) {
    lv_obj_t *btn = lv_obj_create(parent);

    lv_obj_remove_style_all(btn);
    lv_obj_add_style(btn, zmk_display_style(ZMK_STYLE_TOUCH), 0);
    lv_obj_set_size(btn, LV_SIZE_CONTENT, ZMK_GRID_TOUCH_MIN);
    lv_obj_set_style_pad_hor(btn, ZMK_GRID_UNIT * 2, 0);
    lv_obj_set_style_pad_ver(btn, ZMK_GRID_UNIT, 0);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn, button_clicked_cb, LV_EVENT_CLICKED, (void *)action);

    lv_obj_t *lbl = lv_label_create(btn);

    lv_obj_add_style(lbl, zmk_display_style(ZMK_STYLE_TEXT_ON_ACCENT), 0);
    lv_obj_center(lbl);
    lv_label_set_text(lbl, text);
    if (label_out != NULL) {
        *label_out = lbl;
    }
    return btn;
}

/* ------------------------------------------------------------------ */
/* Set pips                                                            */
/* ------------------------------------------------------------------ */

static void pips_rebuild(uint8_t count) {
    if (ui.pips == NULL) {
        return;
    }
    lv_obj_clean(ui.pips);
    for (int i = 0; i < 8 && i < count; i++) {
        lv_obj_t *pip = lv_obj_create(ui.pips);

        lv_obj_remove_style_all(pip);
        lv_obj_set_size(pip, ZMK_GRID_UNIT + ZMK_GRID_UNIT / 2, ZMK_GRID_UNIT + ZMK_GRID_UNIT / 2);
        lv_obj_set_style_radius(pip, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(pip, LV_OPA_COVER, 0);
        ui.pip_objs[i] = pip;
    }
    ui.pip_count = count;
    ui.pips_done_shown = -1;
}

static void pips_style(uint8_t set_idx) {
    if ((int)set_idx == ui.pips_done_shown) {
        return; /* restyling all pips every second is pure churn */
    }
    ui.pips_done_shown = set_idx;

    for (uint8_t i = 0; i < ui.pip_count; i++) {
        if (ui.pip_objs[i] == NULL) {
            continue;
        }
        bool done = i < set_idx;

        zmk_ui_style_toggle(ui.pip_objs[i], zmk_display_style(ZMK_STYLE_ACCENT), done);
        zmk_ui_style_toggle(ui.pip_objs[i], zmk_display_style(ZMK_STYLE_INACTIVE), !done);
    }
}

/* ------------------------------------------------------------------ */
/* 1 Hz refresh                                                        */
/* ------------------------------------------------------------------ */

static void pomo_update(void) {
    struct zmk_pomo_status s;
    int64_t remaining;
    char buf[48];

    zmk_pomo_get_status(&s);
    remaining = zmk_pomo_remaining_ms();

    if (s.state == ZMK_POMO_STATE_IDLE) {
        const struct zmk_pomo_config *c = zmk_pomo_config_get();
        uint16_t secs = c->work_s; /* idle shows the next session's work length */
        snprintk(buf, sizeof(buf), "%02u:%02u", secs / 60, secs % 60);
        zmk_ui_set_text(ui.phase_cache, sizeof(ui.phase_cache), ui.phase_label, "idle");
        if (ui.arc_cache != 0) {
            lv_arc_set_value(ui.ring, 0);
            ui.arc_cache = 0;
        }
    } else {
        uint16_t secs = (uint16_t)((remaining + 999) / 1000);

        snprintk(buf, sizeof(buf), "%02u:%02u", secs / 60, secs % 60);
        zmk_ui_set_text(ui.phase_cache, sizeof(ui.phase_cache), ui.phase_label,
                        s.paused ? "paused" : (s.state == ZMK_POMO_STATE_WORK ? "work" : "rest"));

        /* progress = elapsed / total of the running phase, 0..1000 */
        int64_t elapsed = (s.paused ? s.pause_at_ms : k_uptime_get()) - s.phase_start_ms;
        int64_t total = (int64_t)s.phase_total_s * 1000;
        int32_t permille = total > 0 ? (int32_t)((elapsed * 1000) / total) : 0;

        permille = CLAMP(permille, 0, 1000);
        if (permille != ui.arc_cache) {
            lv_arc_set_value(ui.ring, permille);
            ui.arc_cache = permille;
        }
    }
    zmk_ui_set_text(ui.time_cache, sizeof(ui.time_cache), ui.time_label, buf);

    if (s.sets != ui.pip_count) {
        pips_rebuild(s.sets);
    }
    pips_style(
        s.state == ZMK_POMO_STATE_IDLE ? 0 : s.set_idx - (s.state == ZMK_POMO_STATE_WORK ? 1 : 0));

    /* button visibility per state — flags toggled only on transitions */
    bool idle = s.state == ZMK_POMO_STATE_IDLE;

    if (idle != ui.idle_shown) {
        lv_obj_set_flag(ui.btn_start, LV_OBJ_FLAG_HIDDEN, !idle);
        lv_obj_set_flag(ui.btn_pause, LV_OBJ_FLAG_HIDDEN, idle);
        lv_obj_set_flag(ui.btn_cancel, LV_OBJ_FLAG_HIDDEN, idle);
        ui.idle_shown = idle;
        ui.pause_cache[0] = '\0'; /* force label refresh after re-show */
    }
    if (!idle && s.paused != ui.paused_shown) {
        zmk_ui_set_text(ui.pause_cache, sizeof(ui.pause_cache), ui.btn_label_pause,
                        s.paused ? "Resume" : "Pause");
        ui.paused_shown = s.paused;
    }

    /* settings strip text */
    const struct zmk_pomo_config *c = zmk_pomo_config_get();

    snprintk(buf, sizeof(buf), "%u sets  %u:%02u  %u:%02u  noti %u", c->sets, c->work_s / 60,
             c->work_s % 60, c->rest_s / 60, c->rest_s % 60, c->noti_mode);
    zmk_ui_set_text(ui.cfg_cache, sizeof(ui.cfg_cache), ui.cfg_label, buf);
}

static void pomo_tick(void) {
    static uint32_t samples, cycle_sum;
    uint32_t start = k_cycle_get_32();

    pomo_update();
    cycle_sum += k_cycle_get_32() - start;
    if (++samples >= 600) {
        LOG_INF("pomodoro update avg %u us over %u s",
                (unsigned int)k_cyc_to_us_floor32(cycle_sum / samples), samples);
        samples = cycle_sum = 0;
    }
}

/* ------------------------------------------------------------------ */
/* Settings strip dev-cycle                                            */
/* ------------------------------------------------------------------ */

static uint8_t cycle_u8(const uint8_t *choices, size_t n, uint8_t cur) {
    for (size_t i = 0; i < n; i++) {
        if (choices[i] == cur) {
            return choices[(i + 1) % n];
        }
    }
    return choices[0];
}

static uint16_t cycle_u16(const uint16_t *choices, size_t n, uint16_t cur) {
    for (size_t i = 0; i < n; i++) {
        if (choices[i] == cur) {
            return choices[(i + 1) % n];
        }
    }
    return choices[0];
}

ZMK_UI_WORK_DEFINE(cfg_cycle_work, cfg_cycle_work_cb) {
    struct zmk_pomo_config c = *zmk_pomo_config_get();

    ARG_UNUSED(work);

    switch (cfg_dev_field) {
    case 0:
        c.sets = cycle_u8(sets_choices, ARRAY_SIZE(sets_choices), c.sets);
        break;
    case 1:
        c.work_s = cycle_u16(work_choices, ARRAY_SIZE(work_choices), c.work_s);
        break;
    case 2:
        c.rest_s = cycle_u16(rest_choices, ARRAY_SIZE(rest_choices), c.rest_s);
        break;
    case 3:
        c.noti_mode = cycle_u8(noti_choices, ARRAY_SIZE(noti_choices), c.noti_mode);
        break;
    }
    cfg_dev_field = (cfg_dev_field + 1) % 4;

    zmk_pomo_config_set(&c); /* persists; live for the next phase */
    pomo_update();
}

/* Tap in the settings band (below the buttons) cycles one config field;
 * anywhere else is left to the page dots / swipes. */
#define CFG_BAND_TOP_Y (ZMK_GRID_SCREEN_H - ZMK_GRID_REACH_H / 2)

/* Compile-time bound (see stats_screen.c): the global input callback can
 * fire long before this page is ever created. */
ZMK_UI_TAP_STATE(pomo_tap) = { .page_name = PAGE_NAME, .work = &cfg_cycle_work };

static void pomo_touch_cb(struct input_event *evt, void *user_data) {
    ARG_UNUSED(user_data);

    /* cheap pre-filter before the shared detector posts work: only taps
     * landing in the settings band get through. The detector stores RAW
     * panel coordinates; the band edge is logical, so convert first. */
    if (evt->type == INPUT_EV_KEY && evt->code == INPUT_BTN_TOUCH && evt->value == 0) {
        int32_t ly = pomo_tap.y;
        int32_t lx = pomo_tap.x;

        zmk_ui_touch_to_logical(&lx, &ly);
        if (ly < CFG_BAND_TOP_Y) {
            return;
        }
    }
    zmk_ui_tap_input(&pomo_tap, evt);
}
INPUT_CALLBACK_DEFINE(NULL, pomo_touch_cb, NULL);

/* ------------------------------------------------------------------ */
/* Build                                                               */
/* ------------------------------------------------------------------ */

static lv_obj_t *pomo_build(lv_obj_t *parent) {
    lv_obj_t *col, *right;

    memset(&ui, 0, sizeof(ui));

    ui.root = lv_obj_create(parent);
    lv_obj_remove_style_all(ui.root);
    lv_obj_set_size(ui.root, lv_pct(100), lv_pct(100));

    lv_obj_t *title = zmk_ui_label(ui.root, ZMK_STYLE_TEXT_TITLE);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, ZMK_GRID_MARGIN + ZMK_GRID_UNIT);
    lv_label_set_text(title, "Pomodoro");

    /* Landscape split: the ring owns the left half, the readouts and
     * controls the right column. */
    ui.ring = lv_arc_create(ui.root);
    lv_obj_set_size(ui.ring, RING_SIZE, RING_SIZE);
    lv_obj_align(ui.ring, LV_ALIGN_LEFT_MID, ZMK_GRID_MARGIN, -ZMK_GRID_UNIT * 2);
    lv_arc_set_bg_angles(ui.ring, 0, 360);
    lv_arc_set_rotation(ui.ring, 270);
    lv_arc_set_range(ui.ring, 0, 1000);
    lv_arc_set_value(ui.ring, 0);
    lv_obj_remove_style(ui.ring, NULL, LV_PART_KNOB);
    lv_obj_add_style(ui.ring, zmk_display_style(ZMK_STYLE_RING), LV_PART_MAIN);
    lv_obj_add_style(ui.ring, zmk_display_style(ZMK_STYLE_RING_INDICATOR), LV_PART_INDICATOR);
    lv_obj_remove_flag(ui.ring, LV_OBJ_FLAG_CLICKABLE);

    ui.time_label = zmk_ui_label(ui.root, ZMK_STYLE_TEXT_SUBHERO);
    lv_obj_align_to(ui.time_label, ui.ring, LV_ALIGN_CENTER, 0, 0);

    right = zmk_ui_flex(ui.root, LV_FLEX_FLOW_COLUMN, ZMK_GRID_UNIT);
    lv_obj_align(right, LV_ALIGN_RIGHT_MID, -ZMK_GRID_MARGIN, -ZMK_GRID_UNIT * 4);

    ui.phase_label = zmk_ui_label(right, ZMK_STYLE_TEXT_TITLE);

    ui.pips = zmk_ui_flex(right, LV_FLEX_FLOW_ROW, ZMK_GRID_UNIT);

    /* buttons in the reach band */
    col = zmk_ui_flex(right, LV_FLEX_FLOW_ROW, ZMK_GRID_UNIT * 2);
    ui.btn_start = mk_button(col, "Start", 0, &ui.btn_label_start);
    ui.btn_pause = mk_button(col, "Pause", 1, &ui.btn_label_pause);
    ui.btn_cancel = mk_button(col, "Cancel", 2, NULL);

    ui.cfg_label = zmk_ui_label(ui.root, ZMK_STYLE_TEXT_CAPTION);
    lv_obj_align(ui.cfg_label, LV_ALIGN_BOTTOM_MID, 0, -ZMK_GRID_MARGIN * 3);

    pomo_update();
    zmk_ui_page_timer(PAGE_NAME, 1000, pomo_tick);

    return parent;
}

static lv_obj_t *pomo_page_create(lv_obj_t *parent) { return pomo_build(parent); }

ZMK_DISPLAY_DEFINE_SCREEN(pomodoro, PAGE_NAME, pomo_page_create, 20);
