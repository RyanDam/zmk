/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>

#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zephyr/drivers/display.h>
#include <zephyr/drivers/led.h>
#include <lvgl.h>

#include "theme.h"

#include <zmk/event_manager.h>
#include <zmk/events/activity_state_changed.h>
#include <zmk/display/status_screen.h>

#if IS_ENABLED(CONFIG_ZMK_DISPLAY_SCREEN_MGR)
#include <zmk/display/screens.h>
#endif

static const struct device *display = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));

#if DT_HAS_CHOSEN(zmk_display_led)

static const struct device *display_led = DEVICE_DT_GET(DT_PARENT(DT_CHOSEN(zmk_display_led)));
static const uint8_t display_led_idx = DT_NODE_CHILD_IDX(DT_CHOSEN(zmk_display_led));

#endif

static bool initialized = false;

#if !IS_ENABLED(CONFIG_ZMK_DISPLAY_SCREEN_MGR)
static lv_obj_t *screen;
#endif

__attribute__((weak)) lv_obj_t *zmk_display_status_screen() { return NULL; }

/*
 * Product orientation from the panel node's `rotation` — the same property
 * the display driver transposes frames by and ui_helpers.h remaps touch
 * through. Set before any screen exists so every page lays out directly in
 * logical landscape (LVGL swaps the logical resolution; the physical frame
 * buffer stays the panel's shape, and the driver re-transposes at flush).
 *
 * The pointer path rotates with it for free: lv_indev's pointer processing
 * runs the raw coordinates through lv_display_rotate_point (9.5,
 * lv_indev.c indev_pointer_proc) — so zmk_ui_indev keeps feeding raw panel
 * coordinates. The hardware rotation route (AXS15231B_ROTATION_HW) must NOT
 * call this: there the driver reports swapped caps + orientation, LVGL
 * renders landscape natively, and double-rotating would cancel out.
 */
#if DT_HAS_CHOSEN(zephyr_display) && !IS_ENABLED(CONFIG_AXS15231B_ROTATION_HW)
static void display_setup_rotation(void) {
    lv_display_t *disp = lv_display_get_default();

    if (disp == NULL) {
        return;
    }

    switch (DT_PROP_OR(DT_CHOSEN(zephyr_display), rotation, 0)) {
    case 90:
        lv_display_set_rotation(disp, LV_DISPLAY_ROTATION_90);
        break;
    case 180:
        lv_display_set_rotation(disp, LV_DISPLAY_ROTATION_180);
        break;
    case 270:
        lv_display_set_rotation(disp, LV_DISPLAY_ROTATION_270);
        break;
    default:
        break;
    }
}

#endif

void display_tick_cb(struct k_work *work) { lv_task_handler(); }

K_WORK_DEFINE(display_tick_work, display_tick_cb);

#if IS_ENABLED(CONFIG_ZMK_DISPLAY_WORK_QUEUE_DEDICATED)

K_THREAD_STACK_DEFINE(display_work_stack_area, CONFIG_ZMK_DISPLAY_DEDICATED_THREAD_STACK_SIZE);

static struct k_work_q display_work_q;

#endif

struct k_work_q *zmk_display_work_q() {
#if IS_ENABLED(CONFIG_ZMK_DISPLAY_WORK_QUEUE_DEDICATED)
    return &display_work_q;
#else
    return &k_sys_work_q;
#endif
}

void display_timer_cb() { k_work_submit_to_queue(zmk_display_work_q(), &display_tick_work); }

K_TIMER_DEFINE(display_timer, display_timer_cb, NULL);

void unblank_display_cb(struct k_work *work) {
#if DT_HAS_CHOSEN(zmk_display_led)
    led_on(display_led, display_led_idx);
#endif
    display_blanking_off(display);
#if !IS_ENABLED(CONFIG_ARCH_POSIX)
    k_timer_start(&display_timer, K_MSEC(CONFIG_ZMK_DISPLAY_TICK_PERIOD_MS),
                  K_MSEC(CONFIG_ZMK_DISPLAY_TICK_PERIOD_MS));
#endif // !IS_ENABLED(CONFIG_ARCH_POSIX)
}

#if IS_ENABLED(CONFIG_ZMK_DISPLAY_BLANK_ON_IDLE) || IS_ENABLED(CONFIG_ZMK_DISPLAY_SCREEN_MGR)

void blank_display_cb(struct k_work *work) {
#if !IS_ENABLED(CONFIG_ARCH_POSIX)
    k_timer_stop(&display_timer);
#endif // !IS_ENABLED(CONFIG_ARCH_POSIX)
    display_blanking_on(display);
#if DT_HAS_CHOSEN(zmk_display_led)
    led_off(display_led, display_led_idx);
#endif
}
K_WORK_DEFINE(blank_display_work, blank_display_cb);
K_WORK_DEFINE(unblank_display_work, unblank_display_cb);

/*
 * Start/stop LVGL ticking and panel blanking. Public (zmk/display/screens.h)
 * because the screen manager's idle backlight policy (a08) drives the same
 * pair the BLANK_ON_IDLE activity listener uses; features should prefer
 * zmk_display_blank_lock() over calling these.
 */
void zmk_display_updates_start(void) {
    if (display == NULL) {
        return;
    }

    k_work_submit_to_queue(zmk_display_work_q(), &unblank_display_work);
}

void zmk_display_updates_stop(void) {
    if (display == NULL) {
        return;
    }

    k_work_submit_to_queue(zmk_display_work_q(), &blank_display_work);
}

#endif

int zmk_display_is_initialized() { return initialized; }

static void initialize_theme() {
#if IS_ENABLED(CONFIG_LV_USE_THEME_MONO)
    lv_disp_t *disp = lv_disp_get_default();
    lv_theme_t *theme =
        lv_theme_mono_init(disp, IS_ENABLED(CONFIG_ZMK_DISPLAY_INVERT), CONFIG_LV_FONT_DEFAULT,
                           CONFIG_ZMK_LV_FONT_DEFAULT_SMALL, CONFIG_LV_FONT_DEFAULT);

    lv_display_set_theme(disp, theme);
#endif // CONFIG_LV_USE_THEME_MONO
}

void initialize_display(struct k_work *work) {
    LOG_DBG("");

    if (!device_is_ready(display)) {
        LOG_ERR("Failed to find display device");
        return;
    }

    initialized = true;

#if DT_HAS_CHOSEN(zephyr_display) && !IS_ENABLED(CONFIG_AXS15231B_ROTATION_HW)
    display_setup_rotation();
#endif

    initialize_theme();

#if IS_ENABLED(CONFIG_ZMK_DISPLAY_DESIGN_SYSTEM)
    /* Build the style registry from the theme before any screen is created;
     * screens must reference styles only (src/display/grid.md). */
    zmk_display_theme_init();
#endif

#if IS_ENABLED(CONFIG_ZMK_DISPLAY_SCREEN_MGR)
    zmk_display_screen_mgr_init();
#else
    screen = zmk_display_status_screen();

    if (screen == NULL) {
        LOG_ERR("No status screen provided");
        return;
    }

    lv_scr_load(screen);
#endif

    unblank_display_cb(work);
}

K_WORK_DEFINE(init_work, initialize_display);

int zmk_display_init() {
#if IS_ENABLED(CONFIG_ZMK_DISPLAY_WORK_QUEUE_DEDICATED)
    struct k_work_queue_config config = {
#if IS_ENABLED(CONFIG_THREAD_NAME)
        .name = "display queue",
#endif
    };
    k_work_queue_start(&display_work_q, display_work_stack_area,
                       K_THREAD_STACK_SIZEOF(display_work_stack_area),
                       CONFIG_ZMK_DISPLAY_DEDICATED_THREAD_PRIORITY, &config);
#endif

#if IS_ENABLED(CONFIG_ARCH_POSIX)
    initialize_display(NULL);
#else
    k_work_submit_to_queue(zmk_display_work_q(), &init_work);
#endif

    LOG_DBG("");
    return 0;
}

#if IS_ENABLED(CONFIG_ZMK_DISPLAY_BLANK_ON_IDLE)
int display_event_handler(const zmk_event_t *eh) {
    struct zmk_activity_state_changed *ev = as_zmk_activity_state_changed(eh);
    if (ev == NULL) {
        return -ENOTSUP;
    }

    switch (ev->state) {
    case ZMK_ACTIVITY_ACTIVE:
        zmk_display_updates_start();
        break;
    case ZMK_ACTIVITY_IDLE:
    case ZMK_ACTIVITY_SLEEP:
        zmk_display_updates_stop();
        break;
    default:
        LOG_WRN("Unhandled activity state: %d", ev->state);
        return -EINVAL;
    }
    return 0;
}

ZMK_LISTENER(display, display_event_handler);
ZMK_SUBSCRIPTION(display, zmk_activity_state_changed);

#endif /* IS_ENABLED(CONFIG_ZMK_DISPLAY_BLANK_ON_IDLE) */
