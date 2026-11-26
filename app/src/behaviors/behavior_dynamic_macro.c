/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_dynamic_macro

#include <string.h>

#include <zephyr/device.h>
#include <drivers/behavior.h>
#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zephyr/settings/settings.h>
#include <zmk/behavior.h>
#include <zmk/behavior_queue.h>
#include <zmk/behavior_dynamic_macro.h>

#define DM_COUNT CONFIG_ZMK_DYNAMIC_MACROS_COUNT
#define DM_MAX_STEPS CONFIG_ZMK_DYNAMIC_MACROS_MAX_STEPS

static struct zmk_dynamic_macro_step dynamic_macros[DM_COUNT][DM_MAX_STEPS];
static int zmk_dynamic_macro_step_length[DM_COUNT];
static bool unsaved_changes = false;
static int dirty_macro_indices[DM_COUNT];
static char macro_names[DM_COUNT][ZMK_DYNAMIC_MACRO_NAME_MAX];
static int dirty_name_indices[DM_COUNT];

#define DM_BINDING_SETTINGS_KEY "dynamic_macros/dm/%d/%d" // macro step
#define DL_BINDING_SETTINGS_KEY "dynamic_macros/lm/%d"    // macro lenght
#define DN_BINDING_SETTINGS_KEY "dynamic_macros/dn/%d"    // macro display name

/* Macro-name change listeners (UI re-labeling). */
#define DM_LISTENER_MAX 4
static zmk_dynamic_macro_changed_cb dm_listeners[DM_LISTENER_MAX];

int zmk_dynamic_macro_add_listener(zmk_dynamic_macro_changed_cb cb) {
    for (int i = 0; i < DM_LISTENER_MAX; i++) {
        if (dm_listeners[i] == cb) {
            return 0;
        }
        if (dm_listeners[i] == NULL) {
            dm_listeners[i] = cb;
            return 0;
        }
    }
    return -ENOSPC;
}

static void notify_macro_changed(uint32_t macro_idx) {
    for (int i = 0; i < DM_LISTENER_MAX; i++) {
        if (dm_listeners[i] != NULL) {
            dm_listeners[i](macro_idx);
        }
    }
}

const char *zmk_dynamic_macro_get_name(uint32_t macro_idx) {
    if (macro_idx >= DM_COUNT) {
        return "";
    }
    return macro_names[macro_idx];
}

int zmk_dynamic_macro_set_name(uint32_t macro_idx, const char *name) {
    if (macro_idx >= DM_COUNT || name == NULL ||
        strnlen(name, ZMK_DYNAMIC_MACRO_NAME_MAX) >= ZMK_DYNAMIC_MACRO_NAME_MAX) {
        return -EINVAL;
    }

    if (strcmp(macro_names[macro_idx], name) == 0) {
        return 0;
    }

    strcpy(macro_names[macro_idx], name);
    unsaved_changes = true;
    dirty_name_indices[macro_idx] = 1;
    notify_macro_changed(macro_idx);
    return 0;
}

struct zmk_dynamic_macro_step_setting {
    zmk_behavior_local_id_t behavior_local_id;
    uint32_t param1;
    uint32_t param2;
    uint32_t wait_ms;
    uint8_t mode;
} __packed;

int zmk_dynamic_macro_get_count(void) { return DM_COUNT; }

int zmk_dynamic_macro_get_max_steps(void) { return DM_MAX_STEPS; }

int zmk_dynamic_macro_get_step_length(uint32_t macro_idx) {
    if (macro_idx >= DM_COUNT) {
        return 0;
    }
    return zmk_dynamic_macro_step_length[macro_idx];
}

int zmk_dynamic_macro_set_step_length(uint32_t macro_idx, uint8_t length) {
    if (macro_idx >= DM_COUNT) {
        return -EINVAL;
    }
    zmk_dynamic_macro_step_length[macro_idx] = length;
    unsaved_changes = true;
    dirty_macro_indices[macro_idx] = 1;
    return 0;
}

struct zmk_dynamic_macro_step *zmk_dynamic_macro_get_step(uint32_t macro_idx, uint32_t step_idx) {
    if (macro_idx >= DM_COUNT || step_idx >= DM_MAX_STEPS) {
        LOG_DBG("Invalid macro idx %d/%d and step idx %d/%d", macro_idx, DM_COUNT, step_idx,
                DM_MAX_STEPS);
        return NULL;
    }
    return &dynamic_macros[macro_idx][step_idx];
}

int zmk_dynamic_macro_set_step(uint32_t macro_idx, uint32_t step_idx,
                               struct zmk_dynamic_macro_step step) {
    if (macro_idx >= DM_COUNT || step_idx >= DM_MAX_STEPS) {
        return -EINVAL;
    }

    dynamic_macros[macro_idx][step_idx] = step;
    unsaved_changes = true;
    dirty_macro_indices[macro_idx] = 1;
    return 0;
}

static void queue_macro(struct zmk_behavior_binding_event *event, uint32_t macro_idx) {
    uint8_t macro_length = zmk_dynamic_macro_get_step_length(macro_idx);
    for (int i = 0; i < macro_length; i++) {
        struct zmk_dynamic_macro_step *step = &dynamic_macros[macro_idx][i];

        if (!step->binding.behavior_dev) {
            if (!step->behavior_local_id)
                break;
            const char *behavior_dev =
                zmk_behavior_find_behavior_name_from_local_id(step->behavior_local_id);
            if (!behavior_dev) {
                LOG_ERR("Invalid behavior local id: %d", step->behavior_local_id);
                break;
            }
            step->binding.behavior_dev = behavior_dev;
        }

        switch (step->mode) {
        case MACRO_MODE_TAP:
            zmk_behavior_queue_add(event, step->binding, true,
                                   30); // dynamic macro doesn't have tap time, default to 30ms
            zmk_behavior_queue_add(event, step->binding, false, step->wait_ms);
            break;
        case MACRO_MODE_PRESS:
            zmk_behavior_queue_add(event, step->binding, true, step->wait_ms);
            break;
        case MACRO_MODE_RELEASE:
            zmk_behavior_queue_add(event, step->binding, false, step->wait_ms);
            break;
        }
    }
}

static int on_dynamic_macro_binding_pressed(struct zmk_behavior_binding *binding,
                                            struct zmk_behavior_binding_event event) {
    uint32_t macro_idx = binding->param1;
    if (macro_idx >= DM_COUNT) {
        return -EINVAL;
    }

    queue_macro(&event, macro_idx);
    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_dynamic_macro_binding_released(struct zmk_behavior_binding *binding,
                                             struct zmk_behavior_binding_event event) {
    return ZMK_BEHAVIOR_OPAQUE;
}

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)

static const struct behavior_parameter_value_metadata macro_idx_values[] = {{
    .display_name = "Macro",
    .type = BEHAVIOR_PARAMETER_VALUE_TYPE_RANGE,
    .range = {.min = 0, .max = DM_COUNT - 1},
}};

static const struct behavior_parameter_metadata_set metadata_set[] = {{
    .param1_values = macro_idx_values,
    .param1_values_len = ARRAY_SIZE(macro_idx_values),
}};

static const struct behavior_parameter_metadata metadata = {
    .sets_len = ARRAY_SIZE(metadata_set),
    .sets = metadata_set,
};

#endif // IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)

static const struct behavior_driver_api behavior_dynamic_macro_driver_api = {
    .binding_pressed = on_dynamic_macro_binding_pressed,
    .binding_released = on_dynamic_macro_binding_released,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .parameter_metadata = &metadata,
#endif // IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
};

static int behavior_dynamic_macro_init(const struct device *dev) { return 0; }

BEHAVIOR_DT_INST_DEFINE(0, behavior_dynamic_macro_init, NULL, NULL, NULL, POST_KERNEL,
                        CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &behavior_dynamic_macro_driver_api);

#if IS_ENABLED(CONFIG_SETTINGS)
static int dynamic_macros_handle_set(const char *name, size_t len, settings_read_cb read_cb,
                                     void *cb_arg) {
    const char *next;
    if (settings_name_steq(name, "dm", &next) && next) {
        char *endptr;
        unsigned long macro_idx = strtoul(next, &endptr, 10);
        if (*endptr != '/') {
            return -EINVAL;
        }
        unsigned long step_idx = strtoul(endptr + 1, NULL, 10);

        if (macro_idx >= DM_COUNT || step_idx >= DM_MAX_STEPS) {
            return -EINVAL;
        }

        struct zmk_dynamic_macro_step_setting step_setting;
        if (read_cb(cb_arg, &step_setting, sizeof(step_setting)) != sizeof(step_setting)) {
            return -EINVAL;
        }

        const char *behavior_dev =
            zmk_behavior_find_behavior_name_from_local_id(step_setting.behavior_local_id);
        dynamic_macros[macro_idx][step_idx] = (struct zmk_dynamic_macro_step){
            .binding =
                {
                    .behavior_dev = behavior_dev,
                    .param1 = step_setting.param1,
                    .param2 = step_setting.param2,
                },
            .behavior_local_id = step_setting.behavior_local_id,
            .wait_ms = step_setting.wait_ms,
            .mode = step_setting.mode,
        };
        dirty_macro_indices[macro_idx] = 1;
        return 0;
    } else if (settings_name_steq(name, "lm", &next) && next) {
        char *endptr;
        unsigned long macro_idx = strtoul(next, &endptr, 10);
        if (*endptr != '\0') {
            return -EINVAL;
        }

        if (macro_idx >= DM_COUNT) {
            return -EINVAL;
        }

        uint8_t length;
        if (read_cb(cb_arg, &length, sizeof(length)) != sizeof(length)) {
            return -EINVAL;
        }

        LOG_DBG("Loaded length %d for macro %ld", length, macro_idx);
        zmk_dynamic_macro_step_length[macro_idx] = length;
        return 0;
    } else if (settings_name_steq(name, "dn", &next) && next) {
        char *endptr;
        unsigned long macro_idx = strtoul(next, &endptr, 10);
        if (*endptr != '\0') {
            return -EINVAL;
        }

        if (macro_idx >= DM_COUNT) {
            return -EINVAL;
        }

        ssize_t rlen = read_cb(cb_arg, macro_names[macro_idx], ZMK_DYNAMIC_MACRO_NAME_MAX - 1);
        if (rlen < 0) {
            return -EINVAL;
        }
        macro_names[macro_idx][rlen] = '\0';
        LOG_DBG("Loaded name \"%s\" for macro %ld", macro_names[macro_idx], macro_idx);
        return 0;
    }
    return -ENOENT;
}

static int dynamic_macros_handle_commit(void) {
    // Settings load replaced every name/stage: let UIs relabel.
    notify_macro_changed(ZMK_DYNAMIC_MACRO_ALL);
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(dynamic_macros, "dynamic_macros", NULL, dynamic_macros_handle_set,
                               dynamic_macros_handle_commit, NULL);

int zmk_dynamic_macro_save_changes(void) {
    // Persist changed display names first (empty names are deleted, not stored)
    for (int i = 0; i < DM_COUNT; i++) {
        if (dirty_name_indices[i] == 0) {
            continue;
        }
        dirty_name_indices[i] = 0;

        static char name_setting_name[32];
        snprintf(name_setting_name, sizeof(name_setting_name), DN_BINDING_SETTINGS_KEY, i);

        if (macro_names[i][0] == '\0') {
            settings_delete(name_setting_name);
            continue;
        }

        int ret = settings_save_one(name_setting_name, macro_names[i], strlen(macro_names[i]) + 1);
        if (ret < 0) {
            LOG_ERR("Failed to save name for macro %d. Err code: %d", i, ret);
            return ret;
        }
    }

    for (int i = 0; i < DM_COUNT; i++) {
        if (dirty_macro_indices[i] == 0) {
            continue;
        }

        dirty_macro_indices[i] = 0;

        uint8_t macro_length = zmk_dynamic_macro_get_step_length(i);

        static char length_setting_name[32];
        snprintf(length_setting_name, sizeof(length_setting_name), DL_BINDING_SETTINGS_KEY, i);
        int len_ret = settings_save_one(length_setting_name, &macro_length, sizeof(macro_length));
        if (len_ret < 0) {
            LOG_ERR("Failed to save macro length for macro %s. Err code: %d", length_setting_name,
                    len_ret);
            return -EINVAL;
        }

        for (int j = 0; j < macro_length; j++) {
            struct zmk_dynamic_macro_step *step = &dynamic_macros[i][j];

            static char setting_name[32];
            snprintf(setting_name, sizeof(setting_name), DM_BINDING_SETTINGS_KEY, i, j);

            if (!step->behavior_local_id) {
                if (settings_delete(setting_name) == 0) {
                    LOG_DBG("Deleted setting %s", setting_name);
                }
                continue;
            }

            struct zmk_dynamic_macro_step_setting step_setting = {
                .behavior_local_id = step->behavior_local_id,
                .param1 = step->binding.param1,
                .param2 = step->binding.param2,
                .wait_ms = step->wait_ms,
                .mode = step->mode,
            };

            int ret = settings_save_one(setting_name, &step_setting, sizeof(step_setting));
            if (ret < 0) {
                LOG_ERR("Failed to save setting %s. Err code: %d", setting_name, ret);
                return ret;
            }
        }
    }
    unsaved_changes = false;
    return 0;
}

int zmk_dynamic_macro_discard_changes(void) {
    LOG_DBG("Discarding changes, reloading from settings");
    memset(dynamic_macros, 0, sizeof(dynamic_macros));
    memset(zmk_dynamic_macro_step_length, 0, sizeof(zmk_dynamic_macro_step_length));
    memset(dirty_macro_indices, 0, sizeof(dirty_macro_indices));
    memset(macro_names, 0, sizeof(macro_names));
    memset(dirty_name_indices, 0, sizeof(dirty_name_indices));
    settings_load_subtree("dynamic_macros");
    unsaved_changes = false;
    notify_macro_changed(ZMK_DYNAMIC_MACRO_ALL);
    return 0;
}

int zmk_dynamic_macro_check_unsaved_changes(void) { return unsaved_changes; }

#else

int zmk_dynamic_macro_save_changes(void) { return -ENOTSUP; }
int zmk_dynamic_macro_discard_changes(void) { return -ENOTSUP; }
int zmk_dynamic_macro_check_unsaved_changes(void) { return 0; }

#endif // IS_ENABLED(CONFIG_SETTINGS)