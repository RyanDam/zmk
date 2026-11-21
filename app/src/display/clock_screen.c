/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Clock and calendar status screen for display-centric boards (Coban Tab 1a).
 *
 * Shows the time of day (HH:MM:SS) and the date, refreshed once per second.
 * The board has no battery-backed RTC, no Wi-Fi and BLE only, so there is no
 * real time source: the clock is simulated. It starts from a fixed reference
 * time and advances with the system uptime.
 *
 * With the design system enabled (a07) every color/font/size comes from the
 * style registry (src/display/styles.h + grid.md) — no hardcoded styling in
 * this file; tapping the screen cycles the theme preset to demonstrate live
 * re-theming. Without it, the original hardcoded demo styling remains.
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/input/input.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#if !IS_ENABLED(CONFIG_ZMK_DISPLAY_DESIGN_SYSTEM)
#include <zephyr/random/random.h>
#endif

#include <lvgl.h>

#include <zmk/display.h>
#include <zmk/display/status_screen.h>

#if IS_ENABLED(CONFIG_ZMK_DISPLAY_DESIGN_SYSTEM)
#include "styles.h"
#include "theme.h"
#endif

#if IS_ENABLED(CONFIG_ZMK_DISPLAY_SCREEN_MGR)
#include <zmk/display/screens.h>
#endif

LOG_MODULE_REGISTER(display_clock, CONFIG_ZMK_LOG_LEVEL);

/*
 * Simulated clock reference: 2026-11-14 12:00:00 UTC, in epoch seconds.
 * Displayed time = reference + system uptime.
 */
#define CLOCK_BASE_EPOCH_S 1794657600L

struct clock_time {
    uint16_t hour;
    uint16_t minute;
    uint16_t second;
    uint16_t year;
    uint8_t month;   /* 1-12 */
    uint8_t day;     /* 1-31 */
    uint8_t weekday; /* 0 = Sunday */
};

/*
 * Convert days since 1970-01-01 to a civil date.
 * (Howard Hinnant's days_to_ymd algorithm.)
 */
static void civil_from_days(int32_t z, uint16_t *year, uint8_t *month, uint8_t *day)
{
    z += 719468;
    int32_t era = (z >= 0 ? z : z - 146096) / 146097;
    uint32_t doe = (uint32_t)(z - (int64_t)era * 146097);
    uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    uint32_t y = yoe + (uint32_t)era * 400;
    uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    uint32_t mp = (5 * doy + 2) / 153;
    uint32_t d = doy - (153 * mp + 2) / 5 + 1;
    uint32_t m = (mp < 10) ? mp + 3 : mp - 9;

    *year = (uint16_t)(y + (m <= 2));
    *month = (uint8_t)m;
    *day = (uint8_t)d;
}

static void clock_get_time(struct clock_time *t)
{
    int64_t now = CLOCK_BASE_EPOCH_S + k_uptime_get() / 1000;
    int32_t days = (int32_t)(now / 86400);
    int32_t rem = (int32_t)(now % 86400);

    t->hour = (uint16_t)(rem / 3600);
    t->minute = (uint16_t)((rem % 3600) / 60);
    t->second = (uint16_t)(rem % 60);
    civil_from_days(days, &t->year, &t->month, &t->day);
    /* 1970-01-01 was a Thursday. */
    t->weekday = (uint8_t)(((days % 7) + 4) % 7);
}

static const char *const weekday_names[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
static const char *const month_names[] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
};

#if IS_ENABLED(CONFIG_ZMK_DISPLAY_DESIGN_SYSTEM)

/* --------------------------------------------------------------------- */
/* Design-system screen: styling only via zmk_display_style() (a07).     */
/* --------------------------------------------------------------------- */

static lv_obj_t *clock_label;  /* HH:MM  — ZMK_STYLE_TEXT_HERO     */
static lv_obj_t *seconds_label;/* :SS    — ZMK_STYLE_TEXT_SUBHERO  */
static lv_obj_t *date_label;   /* body, secondary color            */
static lv_obj_t *hint_label;   /* caption                          */

static void clock_update_text(void)
{
    struct clock_time t;
    char buf[32];

    clock_get_time(&t);

    snprintk(buf, sizeof(buf), "%02u:%02u", t.hour, t.minute);
    lv_label_set_text(clock_label, buf);

    snprintk(buf, sizeof(buf), "%02u", t.second);
    lv_label_set_text(seconds_label, buf);

    snprintk(buf, sizeof(buf), "%s, %s %u %u", weekday_names[t.weekday], month_names[t.month - 1],
             t.day, t.year);
    lv_label_set_text(date_label, buf);
}

static void clock_timer_cb(lv_timer_t *timer)
{
    ARG_UNUSED(timer);
    clock_update_text();
}

/*
 * Touch handling: the input callback runs in the input reporting context, so
 * the LVGL work is posted to the display work queue. With the screen manager
 * (a08) this global tap handler is off: every tap AND swipe press would
 * cycle the theme, so theme switching waits for its own control (h04 keys /
 * d05 overlay / a14 RPC).
 */
#if !IS_ENABLED(CONFIG_ZMK_DISPLAY_SCREEN_MGR)
static void clock_theme_cycle_work_cb(struct k_work *work)
{
    ARG_UNUSED(work);

    /* The touch controller reports a phantom touch at power-on, so a cycle
     * work item can run before the screen exists — skip it (the theme
     * itself is always valid; this guard only orders against boot noise). */
    if (clock_label == NULL) {
        return;
    }

    zmk_display_theme_cycle_preset();
}
K_WORK_DEFINE(clock_theme_cycle_work, clock_theme_cycle_work_cb);

static void clock_touch_cb(struct input_event *evt, void *user_data)
{
    ARG_UNUSED(user_data);

    if (evt->type == INPUT_EV_KEY && evt->code == INPUT_BTN_TOUCH && evt->value == 1) {
        k_work_submit_to_queue(zmk_display_work_q(), &clock_theme_cycle_work);
    }
}
INPUT_CALLBACK_DEFINE(NULL, clock_touch_cb, NULL);
#endif /* !CONFIG_ZMK_DISPLAY_SCREEN_MGR */

/* Build the clock widgets into `screen`, which must already carry the
 * ZMK_STYLE_SCREEN style. Shared by the standalone status screen and the
 * screen-manager page registration below. */
static lv_obj_t *clock_build_content(lv_obj_t *screen)
{
    clock_label = lv_label_create(screen);
    lv_obj_add_style(clock_label, zmk_display_style(ZMK_STYLE_TEXT_HERO), 0);
    lv_obj_align(clock_label, LV_ALIGN_CENTER, 0, -ZMK_GRID_UNIT * 5);

    seconds_label = lv_label_create(screen);
    lv_obj_add_style(seconds_label, zmk_display_style(ZMK_STYLE_TEXT_SUBHERO), 0);
    lv_obj_align(seconds_label, LV_ALIGN_CENTER, 0, ZMK_GRID_UNIT * 4);

    date_label = lv_label_create(screen);
    lv_obj_add_style(date_label, zmk_display_style(ZMK_STYLE_TEXT_SECONDARY), 0);
    lv_obj_align(date_label, LV_ALIGN_CENTER, 0, ZMK_GRID_UNIT * 9);

    hint_label = lv_label_create(screen);
    lv_obj_add_style(hint_label, zmk_display_style(ZMK_STYLE_TEXT_CAPTION), 0);
#if IS_ENABLED(CONFIG_ZMK_DISPLAY_SCREEN_MGR)
    /* Above the page dots. */
    lv_obj_align(hint_label, LV_ALIGN_BOTTOM_MID, 0, -ZMK_GRID_MARGIN * 3);
    lv_label_set_text(hint_label, "swipe \xe2\x86\x90 \xe2\x86\x92 to change screens");
#else
    lv_obj_align(hint_label, LV_ALIGN_BOTTOM_MID, 0, -ZMK_GRID_MARGIN);
    lv_label_set_text(hint_label, "tap the screen to change theme");
#endif

    clock_update_text();
    lv_timer_create(clock_timer_cb, 1000, NULL);

    return screen;
}

#if IS_ENABLED(CONFIG_ZMK_DISPLAY_SCREEN_MGR)

static lv_obj_t *clock_page_create(lv_obj_t *parent) { return clock_build_content(parent); }

ZMK_DISPLAY_DEFINE_SCREEN(clock, "clock", clock_page_create, 10);

#else /* standalone status screen (no screen manager) */

lv_obj_t *zmk_display_status_screen(void)
{
    lv_obj_t *screen = lv_obj_create(NULL);

    lv_obj_add_style(screen, zmk_display_style(ZMK_STYLE_SCREEN), 0);
    clock_build_content(screen);

    LOG_INF("Clock status screen ready (simulated time, base 2026-11-14 12:00 UTC)");
    return screen;
}

#endif /* CONFIG_ZMK_DISPLAY_SCREEN_MGR */

#else /* !CONFIG_ZMK_DISPLAY_DESIGN_SYSTEM — original hardcoded demo styling */

static lv_obj_t *clock_label;
static lv_obj_t *date_label;
static lv_obj_t *hint_label;

static void clock_update_text(void)
{
    struct clock_time t;
    char buf[32];

    clock_get_time(&t);

    snprintk(buf, sizeof(buf), "%02u:%02u:%02u", t.hour, t.minute, t.second);
    lv_label_set_text(clock_label, buf);

    snprintk(buf, sizeof(buf), "%s, %s %u %u", weekday_names[t.weekday], month_names[t.month - 1],
             t.day, t.year);
    lv_label_set_text(date_label, buf);
}

static void clock_timer_cb(lv_timer_t *timer)
{
    ARG_UNUSED(timer);
    clock_update_text();
}

/*
 * Touch handling: the input callback runs in the input reporting context, so
 * the LVGL update is posted to the display work queue.
 */
static void clock_random_color_work_cb(struct k_work *work)
{
    ARG_UNUSED(work);

    /* The labels are created by zmk_display_status_screen() in this same
     * work queue. The touch controller reports a phantom touch at power-on,
     * so a color-change work item can run before the screen exists — skip
     * it rather than dereference NULL. */
    if (clock_label == NULL || date_label == NULL) {
        return;
    }

    uint32_t rgb = sys_rand32_get();
    lv_color_t color = lv_color_make(96 + (rgb % 160), 96 + ((rgb >> 8) % 160),
                                     96 + ((rgb >> 16) % 160));

    lv_obj_set_style_text_color(clock_label, color, 0);
    lv_obj_set_style_text_color(date_label, color, 0);
}
K_WORK_DEFINE(clock_random_color_work, clock_random_color_work_cb);

static void clock_touch_cb(struct input_event *evt, void *user_data)
{
    ARG_UNUSED(user_data);

    if (evt->type == INPUT_EV_KEY && evt->code == INPUT_BTN_TOUCH && evt->value == 1) {
        k_work_submit_to_queue(zmk_display_work_q(), &clock_random_color_work);
    }
}
INPUT_CALLBACK_DEFINE(NULL, clock_touch_cb, NULL);

lv_obj_t *zmk_display_status_screen(void)
{
    lv_obj_t *screen = lv_obj_create(NULL);

    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    clock_label = lv_label_create(screen);
    lv_obj_set_style_text_font(clock_label, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(clock_label, lv_color_white(), 0);
    lv_obj_align(clock_label, LV_ALIGN_CENTER, 0, -60);

    date_label = lv_label_create(screen);
    lv_obj_set_style_text_font(date_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(date_label, lv_color_white(), 0);
    lv_obj_align(date_label, LV_ALIGN_CENTER, 0, 10);

    hint_label = lv_label_create(screen);
    lv_obj_set_style_text_font(hint_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(hint_label, lv_color_make(128, 128, 128), 0);
    lv_obj_align(hint_label, LV_ALIGN_BOTTOM_MID, 0, -16);
    lv_label_set_text(hint_label, "tap the screen to change color");

    clock_update_text();
    lv_timer_create(clock_timer_cb, 1000, NULL);

    LOG_INF("Clock status screen ready (simulated time, base 2026-11-14 12:00 UTC)");
    return screen;
}

#endif /* CONFIG_ZMK_DISPLAY_DESIGN_SYSTEM */
