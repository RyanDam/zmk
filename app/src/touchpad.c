/*
 * Copyright (c) 2024 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/device.h>
#include <zephyr/init.h>
#include <zephyr/settings/settings.h>
#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/stdlib.h>

#include <zmk/touchpad.h>
#include <zmk/event_manager.h>
#include <zmk/events/mpr121_gesture_event.h>
#include <zmk/events/mpr121_touch_event.h>
#include <zmk/behavior.h>
#include <zmk/hid.h>
#include <zmk/endpoints.h>
#include <mpr121.h>

#define TP_MODE_KEY "touchpad/mode/%d"
#define TP_BIND_KEY "touchpad/b/%d/%d"
#define TP_SENS_KEY "touchpad/sensitivity"

#define PENDING_ARRAY_SIZE DIV_ROUND_UP(TOUCHPAD_NUM_BINDINGS, 8)

static touchpad_mode_t touchpad_mode[ZMK_KEYMAP_LAYERS_LEN];
static struct zmk_behavior_binding touchpad_bindings[ZMK_KEYMAP_LAYERS_LEN][TOUCHPAD_NUM_BINDINGS];
static struct zmk_behavior_binding touchpad_stock_bindings[ZMK_KEYMAP_LAYERS_LEN]
                                                          [TOUCHPAD_NUM_BINDINGS];
static touchpad_mode_t touchpad_stock_mode[ZMK_KEYMAP_LAYERS_LEN];
static uint8_t touchpad_pending_mode[ZMK_KEYMAP_LAYERS_LEN];
static uint8_t touchpad_pending_bindings[ZMK_KEYMAP_LAYERS_LEN][PENDING_ARRAY_SIZE];

static uint16_t touchpad_sensitivity;
static uint8_t touchpad_pending_sensitivity;

#if DT_NODE_EXISTS(DT_NODELABEL(touchpad))

#define _TP_LAYER_BIND(idx, node)                                                                  \
    {                                                                                              \
        .behavior_dev = DEVICE_DT_NAME(DT_PHANDLE_BY_IDX(node, bindings, idx)),                    \
        .param1 = COND_CODE_0(DT_PHA_HAS_CELL_AT_IDX(node, bindings, idx, param1), (0),            \
                              (DT_PHA_BY_IDX(node, bindings, idx, param1))),                       \
        .param2 = COND_CODE_0(DT_PHA_HAS_CELL_AT_IDX(node, bindings, idx, param2), (0),            \
                              (DT_PHA_BY_IDX(node, bindings, idx, param2))),                       \
    }

#define _TP_LAYER(node)                                                                            \
    {                                                                                              \
        .mode = DT_PROP_OR(node, mode, TOUCHPAD_MODE_GESTURE), .bindings = {                       \
            LISTIFY(TOUCHPAD_NUM_BINDINGS, _TP_LAYER_BIND, (, ), node)                             \
        }                                                                                          \
    }

struct tp_layer_cfg {
    touchpad_mode_t mode;
    struct zmk_behavior_binding bindings[TOUCHPAD_NUM_BINDINGS];
};

static const struct tp_layer_cfg stock_layers[] = {
    DT_FOREACH_CHILD_SEP(DT_NODELABEL(touchpad), _TP_LAYER, (, ))};

static const uint8_t stock_layers_count = DT_CHILD_NUM(DT_NODELABEL(touchpad));

#else
static const uint8_t stock_layers_count = 0;
#endif

static void load_stock_from_dts(void) {
    for (int l = 0; l < ZMK_KEYMAP_LAYERS_LEN; l++) {
        if (l < stock_layers_count) {
            touchpad_stock_mode[l] = stock_layers[l].mode;
            memcpy(touchpad_stock_bindings[l], stock_layers[l].bindings,
                   sizeof(touchpad_stock_bindings[l]));
        } else {
            touchpad_stock_mode[l] = TOUCHPAD_MODE_GESTURE;
            memset(touchpad_stock_bindings[l], 0, sizeof(touchpad_stock_bindings[l]));
        }
        // LOG_DBG("Touchpad load stock layer %d mode %d", l, touchpad_stock_mode[l]);
        // for (int b = 0; b < TOUCHPAD_NUM_BINDINGS; b++) {
        //     LOG_DBG("=== Touchpad bind idx %d dev %s %d %d", b,
        //             touchpad_stock_bindings[l][b].behavior_dev,
        //             touchpad_stock_bindings[l][b].param1, touchpad_stock_bindings[l][b].param2);
        // }
    }
}

/* ===== Public API ===== */

touchpad_mode_t zmk_touchpad_get_mode(zmk_keymap_layer_id_t layer) {
    if (layer >= ZMK_KEYMAP_LAYERS_LEN)
        return TOUCHPAD_MODE_GESTURE;
    // LOG_DBG("Touchpad GET mode layer %d mode %d", layer, touchpad_mode[layer]);
    return touchpad_mode[layer];
}

int zmk_touchpad_set_mode(zmk_keymap_layer_id_t layer, touchpad_mode_t mode) {
    if (layer >= ZMK_KEYMAP_LAYERS_LEN)
        return -EINVAL;
    if (touchpad_mode[layer] == mode)
        return 0;
    touchpad_mode[layer] = mode;
    WRITE_BIT(touchpad_pending_mode[layer], 0, 1);
    // LOG_DBG("Touchpad SET mode layer %d mode %d", layer, mode);
    return 0;
}

const struct zmk_behavior_binding *zmk_touchpad_get_binding(zmk_keymap_layer_id_t layer,
                                                            touchpad_binding_type_t type) {
    if (layer >= ZMK_KEYMAP_LAYERS_LEN || type >= TOUCHPAD_NUM_BINDINGS)
        return NULL;

    struct zmk_behavior_binding *binding = &touchpad_bindings[layer][type];

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_LOCAL_IDS_IN_BINDINGS)
    if (!binding->behavior_dev) {
        const char *name_dev = zmk_behavior_find_behavior_name_from_local_id(binding->local_id);
        binding->behavior_dev = name_dev;
    }
#endif

    // LOG_DBG("Touchpad GET binding layer %d type %d dev %s %d %d", layer, type,
    //         binding->behavior_dev, binding->param1, binding->param2);
    return binding;
}

int zmk_touchpad_set_binding(zmk_keymap_layer_id_t layer, touchpad_binding_type_t type,
                             struct zmk_behavior_binding binding) {
    if (layer >= ZMK_KEYMAP_LAYERS_LEN || type >= TOUCHPAD_NUM_BINDINGS)
        return -EINVAL;
    if (memcmp(&touchpad_bindings[layer][type], &binding, sizeof(binding)) == 0)
        return 0;
    memcpy(&touchpad_bindings[layer][type], &binding, sizeof(binding));
    WRITE_BIT(touchpad_pending_bindings[layer][type / 8], type % 8, 1);
    // LOG_DBG("Touchpad SET binding layer %d type %d dev %s %d %d", layer, type,
    // binding.behavior_dev,
    //         binding.param1, binding.param2);
    return 0;
}

int zmk_touchpad_check_unsaved_changes(void) {
    for (int l = 0; l < ZMK_KEYMAP_LAYERS_LEN; l++) {
        if (touchpad_pending_mode[l])
            return 1;
        for (int b = 0; b < TOUCHPAD_NUM_BINDINGS; b++) {
            if (touchpad_pending_bindings[l][b / 8] & BIT(b % 8))
                return 1;
        }
    }
    if (touchpad_pending_sensitivity)
        return 1;
    return 0;
}

uint16_t zmk_touchpad_get_sensitivity(void) { return mpr121_get_effective_scale(); }

int zmk_touchpad_set_sensitivity(uint16_t sensitivity) {
    if (sensitivity < 100 || sensitivity > 500)
        return -EINVAL;
    if (touchpad_sensitivity == sensitivity)
        return 0;
    touchpad_sensitivity = sensitivity;
    touchpad_pending_sensitivity = 1;
    mpr121_set_movement_scale(sensitivity);
    return 0;
}

/* ===== Event Handlers ===== */

static void invoke_binding(zmk_keymap_layer_id_t layer, touchpad_binding_type_t type) {
    struct zmk_behavior_binding *binding = &touchpad_bindings[layer][type];
    if (!binding->behavior_dev)
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_LOCAL_IDS_IN_BINDINGS)
    {
        const char *name_dev = zmk_behavior_find_behavior_name_from_local_id(binding->local_id);
        if (!name_dev)
            return;
        binding->behavior_dev = name_dev;
    }
#else
        return;
#endif

    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
    if (!dev) {
        LOG_ERR("Binding device not found: %s", binding->behavior_dev);
        return;
    }

    struct zmk_behavior_binding_event event = {
        .layer = layer,
        .position = 0,
        .timestamp = k_uptime_get(),
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
        .source = 0,
#endif
    };

    struct zmk_behavior_binding bind_copy = *binding;
    LOG_DBG("Invoke layer %d binding %d: %s p1=0x%02X p2=0x%02X", layer, type,
            binding->behavior_dev, binding->param1, binding->param2);
    zmk_behavior_invoke_binding(&bind_copy, event, true);
    zmk_behavior_invoke_binding(&bind_copy, event, false);
}

static int touchpad_handler_start(const zmk_event_t *eh) {
    struct zmk_mpr121_touch_start_event *evt = as_zmk_mpr121_touch_start_event(eh);
    if (!evt)
        return ZMK_EV_EVENT_BUBBLE;

    zmk_keymap_layer_index_t layer_idx = zmk_keymap_highest_layer_active();

    if (zmk_touchpad_get_mode(layer_idx) != TOUCHPAD_MODE_MOUSE_SIMULATION) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    LOG_DBG("Touch start: x=%d y=%d", (int)(evt->x * 100), (int)(evt->y * 100));
    invoke_binding(layer_idx, TP_BIND_TOUCH_START);
    return ZMK_EV_EVENT_BUBBLE;
}

static int touchpad_handler_move(const zmk_event_t *eh) {
    struct zmk_mpr121_touch_move_event *evt = as_zmk_mpr121_touch_move_event(eh);
    if (!evt)
        return ZMK_EV_EVENT_BUBBLE;

    zmk_keymap_layer_index_t layer_idx = zmk_keymap_highest_layer_active();

    if (zmk_touchpad_get_mode(layer_idx) != TOUCHPAD_MODE_MOUSE_SIMULATION) {
        return ZMK_EV_EVENT_BUBBLE;
    }

#if IS_ENABLED(CONFIG_ZMK_POINTING)
    zmk_hid_mouse_movement_set(evt->dx, evt->dy);
    zmk_endpoint_send_mouse_report();
    zmk_hid_mouse_movement_set(0, 0);
#endif
    return ZMK_EV_EVENT_BUBBLE;
}

static int touchpad_handler_end(const zmk_event_t *eh) {
    struct zmk_mpr121_touch_end_event *evt = as_zmk_mpr121_touch_end_event(eh);
    if (!evt)
        return ZMK_EV_EVENT_BUBBLE;

    zmk_keymap_layer_index_t layer_idx = zmk_keymap_highest_layer_active();

    if (zmk_touchpad_get_mode(layer_idx) != TOUCHPAD_MODE_MOUSE_SIMULATION) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    LOG_DBG("Touch end: x=%d y=%d dur=%u", (int)(evt->x * 100), (int)(evt->y * 100),
            evt->duration_ms);
    invoke_binding(layer_idx, TP_BIND_TOUCH_END);
    return ZMK_EV_EVENT_BUBBLE;
}

static int touchpad_handler_tap(const zmk_event_t *eh) {
    struct zmk_mpr121_touch_tap_event *evt = as_zmk_mpr121_touch_tap_event(eh);
    if (!evt)
        return ZMK_EV_EVENT_BUBBLE;

    zmk_keymap_layer_index_t layer_idx = zmk_keymap_highest_layer_active();

    LOG_DBG("Tap: x=%d y=%d dur=%u", (int)(evt->x * 100), (int)(evt->y * 100), evt->duration_ms);
    invoke_binding(layer_idx, TP_BIND_TAP);
    return ZMK_EV_EVENT_BUBBLE;
}

static int touchpad_handler_gesture(const zmk_event_t *eh) {
    struct zmk_mpr121_gesture_event *evt = as_zmk_mpr121_gesture_event(eh);
    if (!evt)
        return ZMK_EV_EVENT_BUBBLE;

    zmk_keymap_layer_index_t layer_idx = zmk_keymap_highest_layer_active();

    if (zmk_touchpad_get_mode(layer_idx) != TOUCHPAD_MODE_GESTURE) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    static const char *names[] = {"NONE", "LEFT", "RIGHT", "UP", "DOWN"};
    LOG_DBG("Gesture: %s disp=%u vel=%u", names[evt->gesture_type], evt->displacement,
            evt->velocity);

    if (evt->gesture_type >= 1 && evt->gesture_type <= 4) {
        invoke_binding(layer_idx, TP_BIND_GESTURE_LEFT + (evt->gesture_type - 1));
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(touchpad_listener, touchpad_handler_start);
ZMK_SUBSCRIPTION(touchpad_listener, zmk_mpr121_touch_start_event);

ZMK_LISTENER(touchpad_listener_move, touchpad_handler_move);
ZMK_SUBSCRIPTION(touchpad_listener_move, zmk_mpr121_touch_move_event);

ZMK_LISTENER(touchpad_listener_end, touchpad_handler_end);
ZMK_SUBSCRIPTION(touchpad_listener_end, zmk_mpr121_touch_end_event);

ZMK_LISTENER(touchpad_listener_tap, touchpad_handler_tap);
ZMK_SUBSCRIPTION(touchpad_listener_tap, zmk_mpr121_touch_tap_event);

ZMK_LISTENER(touchpad_listener_gesture, touchpad_handler_gesture);
ZMK_SUBSCRIPTION(touchpad_listener_gesture, zmk_mpr121_gesture_event);

/* ===== Settings Persistence ===== */

struct zmk_behavior_binding_setting {
    zmk_behavior_local_id_t behavior_local_id;
    uint32_t param1;
    uint32_t param2;
} __packed;

int zmk_touchpad_save_changes(void) {
    for (int l = 0; l < ZMK_KEYMAP_LAYERS_LEN; l++) {
        if (touchpad_pending_mode[l]) {
            char key[20];
            sprintf(key, TP_MODE_KEY, l);
            LOG_DBG("Touchpad save mode %s %d", key, touchpad_mode[l]);
            int ret = settings_save_one(key, &touchpad_mode[l], sizeof(touchpad_mode[l]));
            if (ret < 0) {
                LOG_ERR("Failed to save mode for layer %d (%d)", l, ret);
                return ret;
            }
            touchpad_pending_mode[l] = 0;
        }

        for (int b = 0; b < TOUCHPAD_NUM_BINDINGS; b++) {
            if (touchpad_pending_bindings[l][b / 8] & BIT(b % 8)) {
                const struct zmk_behavior_binding *binding = &touchpad_bindings[l][b];
                struct zmk_behavior_binding_setting setting = {
                    .behavior_local_id = zmk_behavior_get_local_id(binding->behavior_dev),
                    .param1 = binding->param1,
                    .param2 = binding->param2,
                };
                size_t len = sizeof(setting);
                char key[20];
                sprintf(key, TP_BIND_KEY, l, b);
                LOG_DBG("Touchpad save binding %s len %d bev id %d bev %s %d %d", key, len,
                        setting.behavior_local_id, binding->behavior_dev, setting.param1,
                        setting.param2);
                int ret = settings_save_one(key, &setting, len);
                if (ret < 0) {
                    LOG_ERR("Failed to save binding (%d)", ret);
                    return ret;
                }
                WRITE_BIT(touchpad_pending_bindings[l][b / 8], b % 8, 0);
            }
        }
    }
    if (touchpad_pending_sensitivity) {
        LOG_DBG("Touchpad save sensitivity %s %u", TP_SENS_KEY, touchpad_sensitivity);
        int ret =
            settings_save_one(TP_SENS_KEY, &touchpad_sensitivity, sizeof(touchpad_sensitivity));
        if (ret < 0) {
            LOG_ERR("Failed to save sensitivity (%d)", ret);
            return ret;
        }
        touchpad_pending_sensitivity = 0;
    }
    return 0;
}

int zmk_touchpad_discard_changes(void) {
    for (int l = 0; l < ZMK_KEYMAP_LAYERS_LEN; l++) {
        touchpad_mode[l] = touchpad_stock_mode[l];
        memcpy(touchpad_bindings[l], touchpad_stock_bindings[l], sizeof(touchpad_bindings[l]));
        touchpad_pending_mode[l] = 0;
        memset(touchpad_pending_bindings[l], 0, PENDING_ARRAY_SIZE);
    }
    touchpad_sensitivity = 0;
    touchpad_pending_sensitivity = 0;
    mpr121_set_movement_scale(0);
    return settings_load_subtree("touchpad");
}

int zmk_touchpad_reset_settings(void) {
    for (int l = 0; l < ZMK_KEYMAP_LAYERS_LEN; l++) {
        char mode_key[20];
        sprintf(mode_key, TP_MODE_KEY, l);
        settings_delete(mode_key);
        for (int b = 0; b < TOUCHPAD_NUM_BINDINGS; b++) {
            char bind_key[20];
            sprintf(bind_key, TP_BIND_KEY, l, b);
            settings_delete(bind_key);
        }
    }
    for (int l = 0; l < ZMK_KEYMAP_LAYERS_LEN; l++) {
        touchpad_mode[l] = touchpad_stock_mode[l];
        memcpy(touchpad_bindings[l], touchpad_stock_bindings[l], sizeof(touchpad_bindings[l]));
    }
    settings_delete(TP_SENS_KEY);
    touchpad_sensitivity = 0;
    touchpad_pending_sensitivity = 0;
    mpr121_set_movement_scale(0);
    return 0;
}

static int touchpad_handle_set(const char *name, size_t len, settings_read_cb read_cb,
                               void *cb_arg) {
    const char *next;

    if (settings_name_steq(name, "mode", &next) && next) {
        char *endptr;
        uint8_t layer = strtoul(next, &endptr, 10);
        if (*endptr != '\0' || layer >= ZMK_KEYMAP_LAYERS_LEN)
            return -EINVAL;
        if (len != sizeof(touchpad_mode[layer]))
            return -EINVAL;
        int err = read_cb(cb_arg, &touchpad_mode[layer], len);
        LOG_DBG("Init touchpad mode, layer %d mode %d", layer, touchpad_mode[layer]);
        if (err <= 0)
            return err;
    } else if (settings_name_steq(name, "b", &next) && next) {
        char *endptr;
        uint8_t layer = strtoul(next, &endptr, 10);
        if (*endptr != '/' || layer >= ZMK_KEYMAP_LAYERS_LEN)
            return -EINVAL;
        uint8_t bind_idx = strtoul(endptr + 1, &endptr, 10);
        if (*endptr != '\0' || bind_idx >= TOUCHPAD_NUM_BINDINGS)
            return -EINVAL;
        if (len > sizeof(struct zmk_behavior_binding_setting))
            return -EINVAL;

        struct zmk_behavior_binding_setting setting = {0};
        int err = read_cb(cb_arg, &setting, len);
        if (err <= 0)
            return err;

        const char *name_dev =
            zmk_behavior_find_behavior_name_from_local_id(setting.behavior_local_id);

        touchpad_bindings[layer][bind_idx] = (struct zmk_behavior_binding){
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_LOCAL_IDS_IN_BINDINGS)
            .local_id = setting.behavior_local_id,
#endif
            .behavior_dev = name_dev,
            .param1 = setting.param1,
            .param2 = setting.param2,
        };
        LOG_DBG(
            "Init touchpad binding, layer %d bind_idx %d, name bev %s bev id %d bev %s, %d, % d ",
            layer, bind_idx, name_dev, setting.behavior_local_id,
            touchpad_bindings[layer][bind_idx].behavior_dev,
            touchpad_bindings[layer][bind_idx].param1, touchpad_bindings[layer][bind_idx].param2);
    } else if (strcmp(name, "sensitivity") == 0) {
        if (len != sizeof(touchpad_sensitivity))
            return -EINVAL;
        int err = read_cb(cb_arg, &touchpad_sensitivity, len);
        if (err <= 0)
            return err;
        LOG_DBG("Init touchpad sensitivity %u", touchpad_sensitivity);
    }
    return 0;
}

static int touchpad_handle_commit(void) {
    for (int l = 0; l < ZMK_KEYMAP_LAYERS_LEN; l++) {
        touchpad_stock_mode[l] = touchpad_mode[l];
        memcpy(touchpad_stock_bindings[l], touchpad_bindings[l],
               sizeof(touchpad_stock_bindings[l]));
    }
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(touchpad_settings, "touchpad", NULL, touchpad_handle_set,
                               touchpad_handle_commit, NULL);

static int touchpad_init(void) {
    load_stock_from_dts();

    for (int l = 0; l < ZMK_KEYMAP_LAYERS_LEN; l++) {
        touchpad_mode[l] = touchpad_stock_mode[l];
        memcpy(touchpad_bindings[l], touchpad_stock_bindings[l], sizeof(touchpad_bindings[l]));
    }

    int ret = settings_load_subtree("touchpad");
    if (ret < 0) {
        LOG_WRN("Failed to load touchpad settings (%d)", ret);
    }

    if (touchpad_sensitivity > 0) {
        mpr121_set_movement_scale(touchpad_sensitivity);
    }

    LOG_INF("Touchpad module initialized");
    return 0;
}

SYS_INIT(touchpad_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
