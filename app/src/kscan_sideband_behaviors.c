/*
 * Copyright (c) 2023 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_kscan_sideband_behaviors

#include <zephyr/device.h>
#include <drivers/behavior.h>
#include <zephyr/input/input.h>
#include <zephyr/logging/log.h>
#include <zephyr/pm/device.h>

#include <zmk/event_manager.h>
#include <zmk/behavior.h>
#include <zmk/keymap.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

struct ksbb_entry {
    struct zmk_behavior_binding binding;
    uint8_t row;
    uint8_t column;
};

struct ksbb_config {
    const struct device *kscan;
    bool auto_enable;
    struct ksbb_entry *entries;
    size_t entries_len;
};

struct ksbb_data {
    uint32_t row;
    uint32_t column;
    bool enabled;
};

struct ksbb_entry *find_sideband_behavior(const struct device *dev, uint32_t row, uint32_t column) {
    const struct ksbb_config *cfg = dev->config;

    for (int e = 0; e < cfg->entries_len; e++) {
        struct ksbb_entry *candidate = &cfg->entries[e];

        if (candidate->row == row && candidate->column == column) {
            return candidate;
        }
    }

    return NULL;
}

static void ksbb_inner_input_callback(struct input_event *evt, void *user_data) {
    const struct device *ksbb = user_data;
    struct ksbb_data *data = ksbb->data;

    switch (evt->type) {
    case INPUT_EV_ABS:
        if (evt->code == INPUT_ABS_X) {
            data->column = evt->value;
        } else if (evt->code == INPUT_ABS_Y) {
            data->row = evt->value;
        }
        break;
    case INPUT_EV_KEY:
        if (evt->code != INPUT_BTN_TOUCH) {
            return;
        }

        if (!data->enabled) {
            return;
        }

        struct ksbb_entry *entry = find_sideband_behavior(ksbb, data->row, data->column);
        if (entry) {
            struct zmk_behavior_binding_event event = {.position = INT32_MAX,
                                                       .timestamp = k_uptime_get()};

            if (evt->value) {
                behavior_keymap_binding_pressed(&entry->binding, event);
            } else {
                behavior_keymap_binding_released(&entry->binding, event);
            }
        }

        // Forward the event so the outer consumer (e.g. physical layouts) sees it.
        input_report_abs(ksbb, INPUT_ABS_X, data->column, false, K_NO_WAIT);
        input_report_abs(ksbb, INPUT_ABS_Y, data->row, false, K_NO_WAIT);
        input_report_key(ksbb, INPUT_BTN_TOUCH, evt->value, true, K_NO_WAIT);
        break;
    default:
        break;
    }
}

static int ksbb_enable(const struct device *dev) {
    struct ksbb_data *data = dev->data;
    const struct ksbb_config *config = dev->config;
    data->enabled = true;

#if IS_ENABLED(CONFIG_PM_DEVICE_RUNTIME)
    if (!pm_device_runtime_is_enabled(dev) && pm_device_runtime_is_enabled(config->kscan)) {
        pm_device_runtime_get(config->kscan);
    }
#elif IS_ENABLED(CONFIG_PM_DEVICE)
    if (pm_device_wakeup_is_capable(config->kscan)) {
        pm_device_wakeup_enable(config->kscan, true);
    }
    pm_device_action_run(config->kscan, PM_DEVICE_ACTION_RESUME);
#endif // IS_ENABLED(CONFIG_PM_DEVICE)

    return 0;
}

static int ksbb_disable(const struct device *dev) {
    struct ksbb_data *data = dev->data;
    const struct ksbb_config *config = dev->config;
    data->enabled = false;

#if IS_ENABLED(CONFIG_PM_DEVICE_RUNTIME)
    if (!pm_device_runtime_is_enabled(dev) && pm_device_runtime_is_enabled(config->kscan)) {
        pm_device_runtime_put(config->kscan);
    }
#elif IS_ENABLED(CONFIG_PM_DEVICE)
    if (pm_device_wakeup_is_capable(config->kscan) && !pm_device_wakeup_is_enabled(dev) &&
        pm_device_wakeup_is_enabled(config->kscan)) {
        pm_device_wakeup_enable(config->kscan, false);
    }
    pm_device_action_run(config->kscan, PM_DEVICE_ACTION_SUSPEND);
#endif // IS_ENABLED(CONFIG_PM_DEVICE)

    return 0;
}

#if IS_ENABLED(CONFIG_PM_DEVICE)

static int ksbb_pm_action(const struct device *dev, enum pm_device_action action) {
    switch (action) {
    case PM_DEVICE_ACTION_SUSPEND:
        return ksbb_disable(dev);
    case PM_DEVICE_ACTION_RESUME:
        return ksbb_enable(dev);
    default:
        return -ENOTSUP;
    }
}

#endif // IS_ENABLED(CONFIG_PM_DEVICE)

static int ksbb_init(const struct device *dev) {
    const struct ksbb_config *config = dev->config;

    if (!device_is_ready(config->kscan)) {
        LOG_ERR("input device %s is not ready", config->kscan->name);
        return -ENODEV;
    }

#if IS_ENABLED(CONFIG_PM_DEVICE)
    if (!config->auto_enable) {
        pm_device_init_suspended(dev);
    }
#else
    // Without PM there is no consumer to start us; the inner input device
    // emits autonomously, so the sideband is always active.
    ksbb_enable(dev);
#endif

    return 0;
}

#define ENTRY(e)                                                                                   \
    {                                                                                              \
        .row = DT_PROP(e, row), .column = DT_PROP(e, column),                                      \
        .binding = ZMK_KEYMAP_EXTRACT_BINDING(0, e),                                               \
    }

#define KSBB_INST(n)                                                                               \
    COND_CODE_1(DT_INST_PROP_OR(n, auto_enable, false), (static int ksbb_auto_enable_##n(void) {   \
                    const struct device *dev = DEVICE_DT_GET(DT_DRV_INST(n));                      \
                    COND_CODE_1(IS_ENABLED(CONFIG_PM_DEVICE),                                      \
                                (ksbb_pm_action(dev, PM_DEVICE_ACTION_RESUME);), ())               \
                    return 0;                                                                      \
                } SYS_INIT(ksbb_auto_enable_##n, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);), \
                ())                                                                                \
    static struct ksbb_entry entries_##n[] = {                                                     \
        DT_INST_FOREACH_CHILD_STATUS_OKAY_SEP(n, ENTRY, (, ))};                                    \
    const struct ksbb_config ksbb_config_##n = {                                                   \
        .kscan = DEVICE_DT_GET(DT_INST_PHANDLE(n, kscan)),                                         \
        .auto_enable = DT_INST_PROP_OR(n, auto_enable, false),                                     \
        .entries = entries_##n,                                                                    \
        .entries_len = ARRAY_SIZE(entries_##n),                                                    \
    };                                                                                             \
    struct ksbb_data ksbb_data_##n = {};                                                           \
    PM_DEVICE_DT_INST_DEFINE(n, ksbb_pm_action);                                                   \
    /* Defined directly (like INPUT_CALLBACK_DEFINE_NAMED) because that macro's name               \
     * argument is in a ## context and would not expand a computed name. */                        \
    static const STRUCT_SECTION_ITERABLE(input_callback, _input_callback___zmk_ksbb_cb_##n) = {    \
        .dev = DEVICE_DT_GET(DT_INST_PHANDLE(n, kscan)),                                           \
        .callback = ksbb_inner_input_callback,                                                     \
        .user_data = (void *)DEVICE_DT_GET(DT_DRV_INST(n)),                                        \
    };                                                                                             \
    DEVICE_DT_INST_DEFINE(n, ksbb_init, PM_DEVICE_DT_INST_GET(n), &ksbb_data_##n,                  \
                          &ksbb_config_##n, POST_KERNEL,                                           \
                          CONFIG_ZMK_KSCAN_SIDEBAND_BEHAVIORS_INIT_PRIORITY, NULL);

DT_INST_FOREACH_STATUS_OKAY(KSBB_INST)
