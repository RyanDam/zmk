/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_kscan_composite

#include "kscan_input.h"

#include <zephyr/device.h>
#include <zephyr/pm/device.h>
#include <zephyr/input/input.h>
#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

struct kscan_composite_child_config {
    const struct device *child;
    uint8_t row_offset;
    uint8_t column_offset;
};

#define CHILD_CONFIG(inst)                                                                         \
    {.child = DEVICE_DT_GET(DT_PHANDLE(inst, kscan)),                                              \
     .row_offset = DT_PROP(inst, row_offset),                                                      \
     .column_offset = DT_PROP_OR(inst, col_offset, DT_PROP(inst, column_offset))},

struct kscan_composite_config {
    const struct kscan_composite_child_config *children;
    size_t children_len;
};

struct kscan_composite_data {
    const struct device *dev;
    /** Row/column accumulated from the child's ABS events, consumed on BTN_TOUCH. */
    uint32_t row;
    uint32_t column;
};

static void kscan_composite_child_input_cb(struct input_event *evt, void *user_data) {
    const struct device *dev = user_data;
    struct kscan_composite_data *data = dev->data;

    switch (evt->type) {
    case INPUT_EV_ABS:
        if (evt->code == INPUT_ABS_X) {
            data->column = evt->value;
        } else if (evt->code == INPUT_ABS_Y) {
            data->row = evt->value;
        }
        break;
    case INPUT_EV_KEY:
        if (evt->code == INPUT_BTN_TOUCH) {
            const struct kscan_composite_config *cfg = dev->config;

            for (int c = 0; c < cfg->children_len; c++) {
                const struct kscan_composite_child_config *child_cfg = &cfg->children[c];

                if (child_cfg->child != evt->dev) {
                    continue;
                }

                zmk_kscan_input_report(dev, data->row + child_cfg->row_offset,
                                       data->column + child_cfg->column_offset, evt->value);
            }
        }
        break;
    default:
        break;
    }
}

static int kscan_composite_resume_children(const struct device *dev) {
    const struct kscan_composite_config *cfg = dev->config;

    for (int i = 0; i < cfg->children_len; i++) {
        const struct kscan_composite_child_config *child_cfg = &cfg->children[i];

#if IS_ENABLED(CONFIG_PM_DEVICE_RUNTIME) || IS_ENABLED(CONFIG_PM_DEVICE)
        if (pm_device_wakeup_is_enabled(dev) && pm_device_wakeup_is_capable(child_cfg->child) &&
            !pm_device_wakeup_enable(child_cfg->child, true)) {
            LOG_ERR("Failed to enable wakeup for %s", child_cfg->child->name);
        }
#endif // IS_ENABLED(CONFIG_PM_DEVICE_RUNTIME) || IS_ENABLED(CONFIG_PM_DEVICE)

#if IS_ENABLED(CONFIG_PM_DEVICE_RUNTIME)
        if (!pm_device_runtime_is_enabled(dev) && pm_device_runtime_is_enabled(child_cfg->child)) {
            pm_device_runtime_get(child_cfg->child);
        }
#elif IS_ENABLED(CONFIG_PM_DEVICE)
        pm_device_action_run(child_cfg->child, PM_DEVICE_ACTION_RESUME);
#endif // IS_ENABLED(CONFIG_PM_DEVICE)
    }
    return 0;
}

static int kscan_composite_suspend_children(const struct device *dev) {
    const struct kscan_composite_config *cfg = dev->config;

    for (int i = 0; i < cfg->children_len; i++) {
        const struct kscan_composite_child_config *child_cfg = &cfg->children[i];

#if IS_ENABLED(CONFIG_PM_DEVICE_RUNTIME) || IS_ENABLED(CONFIG_PM_DEVICE)
        if (pm_device_wakeup_is_capable(child_cfg->child) &&
            pm_device_wakeup_is_enabled(child_cfg->child) &&
            !pm_device_wakeup_enable(child_cfg->child, false)) {
            LOG_ERR("Failed to disable wakeup for %s", child_cfg->child->name);
        }
#endif // IS_ENABLED(CONFIG_PM_DEVICE_RUNTIME) || IS_ENABLED(CONFIG_PM_DEVICE)

#if IS_ENABLED(CONFIG_PM_DEVICE_RUNTIME)
        if (!pm_device_runtime_is_enabled(dev) && pm_device_runtime_is_enabled(child_cfg->child)) {
            pm_device_runtime_put(child_cfg->child);
        }
#elif IS_ENABLED(CONFIG_PM_DEVICE)
        pm_device_action_run(child_cfg->child, PM_DEVICE_ACTION_SUSPEND);
#endif // IS_ENABLED(CONFIG_PM_DEVICE)
    }
    return 0;
}

static int kscan_composite_init(const struct device *dev) {
    struct kscan_composite_data *data = dev->data;

    data->dev = dev;

#if IS_ENABLED(CONFIG_PM_DEVICE)
    pm_device_init_suspended(dev);
#endif

    return 0;
}

#if IS_ENABLED(CONFIG_PM_DEVICE)

static int kscan_composite_pm_action(const struct device *dev, enum pm_device_action action) {
    switch (action) {
    case PM_DEVICE_ACTION_SUSPEND:
        return kscan_composite_suspend_children(dev);
    case PM_DEVICE_ACTION_RESUME:
        return kscan_composite_resume_children(dev);
    default:
        return -ENOTSUP;
    }
}

#endif // IS_ENABLED(CONFIG_PM_DEVICE)

/**
 * Register a callback for one child's inner input device, forwarding to the parent
 * composite. The section entry is defined directly (like INPUT_CALLBACK_DEFINE_NAMED)
 * because that macro's name argument is in a ## context and would not expand a
 * computed name; the child node ID is pasted instead.
 */
#define KSCAN_COMP_CHILD_CB(child_id)                                                              \
    COND_CODE_1(DT_NODE_HAS_PROP(child_id, kscan),                                                 \
                (static const STRUCT_SECTION_ITERABLE(input_callback,                              \
                    _input_callback___zmk_kscan_comp_cb_##child_id) = {                            \
                    .dev = DEVICE_DT_GET(DT_PHANDLE(child_id, kscan)),                             \
                    .callback = kscan_composite_child_input_cb,                                    \
                    .user_data = (void *)DEVICE_DT_GET(DT_PARENT(child_id)),                       \
                };),                                                                               \
                ())

#define KSCAN_COMP_DEV(n)                                                                          \
    static const struct kscan_composite_child_config kscan_composite_children_##n[] = {            \
        DT_INST_FOREACH_CHILD(n, CHILD_CONFIG)};                                                   \
    static const struct kscan_composite_config kscan_composite_config_##n = {                      \
        .children = kscan_composite_children_##n,                                                  \
        .children_len = ARRAY_SIZE(kscan_composite_children_##n),                                  \
    };                                                                                             \
    static struct kscan_composite_data kscan_composite_data_##n;                                   \
    PM_DEVICE_DT_INST_DEFINE(n, kscan_composite_pm_action);                                        \
    DT_INST_FOREACH_CHILD_SEP(n, KSCAN_COMP_CHILD_CB, (;));                                        \
    DEVICE_DT_INST_DEFINE(n, kscan_composite_init, PM_DEVICE_DT_INST_GET(n),                       \
                          &kscan_composite_data_##n, &kscan_composite_config_##n, POST_KERNEL,     \
                          CONFIG_ZMK_KSCAN_COMPOSITE_INIT_PRIORITY, NULL);

DT_INST_FOREACH_STATUS_OKAY(KSCAN_COMP_DEV)
