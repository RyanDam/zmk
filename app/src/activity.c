/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/device.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/poweroff.h>

#include <errno.h>

#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/event_manager.h>
#include <zmk/events/activity_state_changed.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/events/sensor_event.h>

#include <zmk/pm.h>

#include <zmk/activity.h>

#if IS_ENABLED(CONFIG_ZMK_USB_DEVICE)
#include <zmk/usb.h>
#endif

#if IS_ENABLED(CONFIG_ZMK_POINTING)
#include <zephyr/input/input.h>
#endif

bool is_usb_power_present(void) {
#if IS_ENABLED(CONFIG_ZMK_USB_DEVICE)
    return zmk_usb_is_powered();
#else
    return false;
#endif /* IS_ENABLED(CONFIG_ZMK_USB_DEVICE) */
}

static enum zmk_activity_state activity_state;

static uint32_t activity_last_uptime;

#define DEFAULT_IDLE_TIMEOUT_MS CONFIG_ZMK_IDLE_TIMEOUT

#if IS_ENABLED(CONFIG_ZMK_SLEEP)
#define DEFAULT_SLEEP_ENABLED true
#define DEFAULT_SLEEP_TIMEOUT_MS CONFIG_ZMK_IDLE_SLEEP_TIMEOUT
#else
#define DEFAULT_SLEEP_ENABLED false
#define DEFAULT_SLEEP_TIMEOUT_MS 0
#endif

#define ACTIVITY_SETTINGS_SUBTREE "activity"
#define ACTIVITY_SETTINGS_ENABLED_KEY "enabled"
#define ACTIVITY_SETTINGS_IDLE_TIMEOUT_KEY "idle_timeout_ms"
#define ACTIVITY_SETTINGS_SLEEP_TIMEOUT_KEY "sleep_timeout_ms"

static struct zmk_activity_config activity_config = {
    .sleep_enabled = DEFAULT_SLEEP_ENABLED,
    .idle_timeout_ms = DEFAULT_IDLE_TIMEOUT_MS,
    .sleep_timeout_ms = DEFAULT_SLEEP_TIMEOUT_MS,
};

int raise_event(void) {
    return raise_zmk_activity_state_changed(
        (struct zmk_activity_state_changed){.state = activity_state});
}

int set_state(enum zmk_activity_state state) {
    if (activity_state == state)
        return 0;

    activity_state = state;
    return raise_event();
}

enum zmk_activity_state zmk_activity_get_state(void) { return activity_state; }

struct zmk_activity_config zmk_activity_get_config(void) { return activity_config; }

static int activity_save_config(void) {
#if IS_ENABLED(CONFIG_SETTINGS)
    int ret =
        settings_save_one(ACTIVITY_SETTINGS_SUBTREE "/" ACTIVITY_SETTINGS_ENABLED_KEY,
                          &activity_config.sleep_enabled, sizeof(activity_config.sleep_enabled));
    if (ret < 0) {
        return ret;
    }

    ret = settings_save_one(ACTIVITY_SETTINGS_SUBTREE "/" ACTIVITY_SETTINGS_IDLE_TIMEOUT_KEY,
                            &activity_config.idle_timeout_ms,
                            sizeof(activity_config.idle_timeout_ms));
    if (ret < 0) {
        return ret;
    }

    ret = settings_save_one(ACTIVITY_SETTINGS_SUBTREE "/" ACTIVITY_SETTINGS_SLEEP_TIMEOUT_KEY,
                            &activity_config.sleep_timeout_ms,
                            sizeof(activity_config.sleep_timeout_ms));
    if (ret < 0) {
        return ret;
    }
#endif

    return 0;
}

int zmk_activity_set_config(const struct zmk_activity_config *config) {
    if (!config) {
        return -EINVAL;
    }

    if (config->idle_timeout_ms == 0) {
        return -EINVAL;
    }

    if (config->sleep_enabled) {
        if (!IS_ENABLED(CONFIG_ZMK_SLEEP)) {
            return -ENOTSUP;
        }

        if (config->sleep_timeout_ms == 0) {
            return -EINVAL;
        }
    }

    activity_config = *config;

    // Reset the inactivity timer so applying settings does not immediately
    // transition to idle or sleep.
    activity_last_uptime = k_uptime_get();

    return activity_save_config();
}

int zmk_activity_reset_config(void) {
    activity_config = (struct zmk_activity_config){
        .sleep_enabled = DEFAULT_SLEEP_ENABLED,
        .idle_timeout_ms = DEFAULT_IDLE_TIMEOUT_MS,
        .sleep_timeout_ms = DEFAULT_SLEEP_TIMEOUT_MS,
    };

    activity_last_uptime = k_uptime_get();

#if IS_ENABLED(CONFIG_SETTINGS)
    settings_delete(ACTIVITY_SETTINGS_SUBTREE "/" ACTIVITY_SETTINGS_ENABLED_KEY);
    settings_delete(ACTIVITY_SETTINGS_SUBTREE "/" ACTIVITY_SETTINGS_IDLE_TIMEOUT_KEY);
    settings_delete(ACTIVITY_SETTINGS_SUBTREE "/" ACTIVITY_SETTINGS_SLEEP_TIMEOUT_KEY);
#endif

    return 0;
}

static int note_activity(void) {
    activity_last_uptime = k_uptime_get();

    return set_state(ZMK_ACTIVITY_ACTIVE);
}

static int activity_event_listener(const zmk_event_t *eh) { return note_activity(); }

void activity_work_handler(struct k_work *work) {
    int32_t current = k_uptime_get();
    int32_t inactive_time = current - activity_last_uptime;
#if IS_ENABLED(CONFIG_ZMK_SLEEP)
    if (activity_config.sleep_enabled && activity_config.sleep_timeout_ms > 0 &&
        inactive_time > activity_config.sleep_timeout_ms && !is_usb_power_present()) {
        // Put devices in suspend power mode before sleeping
        set_state(ZMK_ACTIVITY_SLEEP);

        if (zmk_pm_suspend_devices() < 0) {
            LOG_ERR("Failed to suspend all the devices");
            zmk_pm_resume_devices();
            return;
        }

        sys_poweroff();
    } else
#endif /* IS_ENABLED(CONFIG_ZMK_SLEEP) */
        if (inactive_time > activity_config.idle_timeout_ms) {
            set_state(ZMK_ACTIVITY_IDLE);
        }
}

K_WORK_DEFINE(activity_work, activity_work_handler);

void activity_expiry_function(struct k_timer *_timer) { k_work_submit(&activity_work); }

K_TIMER_DEFINE(activity_timer, activity_expiry_function, NULL);

#if IS_ENABLED(CONFIG_SETTINGS)

static int activity_settings_load_cb(const char *name, size_t len, settings_read_cb read_cb,
                                     void *cb_arg) {
    if (settings_name_steq(name, ACTIVITY_SETTINGS_ENABLED_KEY, NULL)) {
        if (len != sizeof(activity_config.sleep_enabled)) {
            return -EINVAL;
        }

        bool enabled;
        ssize_t ret = read_cb(cb_arg, &enabled, sizeof(enabled));
        if (ret < 0) {
            return ret;
        }

        activity_config.sleep_enabled = enabled && IS_ENABLED(CONFIG_ZMK_SLEEP);
        return 0;
    }

    if (settings_name_steq(name, ACTIVITY_SETTINGS_IDLE_TIMEOUT_KEY, NULL)) {
        if (len != sizeof(activity_config.idle_timeout_ms)) {
            return -EINVAL;
        }

        ssize_t ret = read_cb(cb_arg, &activity_config.idle_timeout_ms,
                              sizeof(activity_config.idle_timeout_ms));
        return ret < 0 ? ret : 0;
    }

    if (settings_name_steq(name, ACTIVITY_SETTINGS_SLEEP_TIMEOUT_KEY, NULL)) {
        if (len != sizeof(activity_config.sleep_timeout_ms)) {
            return -EINVAL;
        }

        ssize_t ret = read_cb(cb_arg, &activity_config.sleep_timeout_ms,
                              sizeof(activity_config.sleep_timeout_ms));
        return ret < 0 ? ret : 0;
    }

    return -ENOENT;
}

static int activity_settings_commit(void) {
    if (activity_config.idle_timeout_ms == 0) {
        activity_config.idle_timeout_ms = DEFAULT_IDLE_TIMEOUT_MS;
    }

    if (!IS_ENABLED(CONFIG_ZMK_SLEEP)) {
        activity_config.sleep_enabled = false;
        activity_config.sleep_timeout_ms = 0;
    } else if (activity_config.sleep_enabled && activity_config.sleep_timeout_ms == 0) {
        activity_config.sleep_timeout_ms = DEFAULT_SLEEP_TIMEOUT_MS;
    }

    activity_last_uptime = k_uptime_get();

    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(activity, ACTIVITY_SETTINGS_SUBTREE, NULL, activity_settings_load_cb,
                               activity_settings_commit, NULL);

#endif // IS_ENABLED(CONFIG_SETTINGS)

static int activity_init(void) {
    activity_last_uptime = k_uptime_get();

    k_timer_start(&activity_timer, K_SECONDS(1), K_SECONDS(1));
    return 0;
}

ZMK_LISTENER(activity, activity_event_listener);
ZMK_SUBSCRIPTION(activity, zmk_position_state_changed);
ZMK_SUBSCRIPTION(activity, zmk_sensor_event);

#if IS_ENABLED(CONFIG_ZMK_POINTING)

static void note_activity_work_cb(struct k_work *_work) { note_activity(); }

K_WORK_DEFINE(note_activity_work, note_activity_work_cb);

static void activity_input_listener(struct input_event *ev, void *user_data) {
    k_work_submit(&note_activity_work);
}

INPUT_CALLBACK_DEFINE(NULL, activity_input_listener, NULL);

#endif

SYS_INIT(activity_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
