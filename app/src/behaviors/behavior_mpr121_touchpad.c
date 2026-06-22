/*
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <drivers/behavior.h>
#include <zmk/event_manager.h>
#include <zmk/events/mpr121_gesture_event.h>
#include <zmk/events/mpr121_touch_event.h>
#include <zmk/behavior.h>
#include <zmk/hid.h>
#include <zmk/endpoints.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define MPR121_TOUCHPAD_NUM_BINDINGS 8

#define TOUCHPAD_NODE_ID DT_NODELABEL(mpr121_touchpad_behavior)

#if DT_NODE_EXISTS(TOUCHPAD_NODE_ID)

#define _TP_BIND(idx)                                                                              \
    {                                                                                              \
        .behavior_dev = DEVICE_DT_NAME(DT_PHANDLE_BY_IDX(TOUCHPAD_NODE_ID, bindings, idx)),        \
        .param1 = COND_CODE_0(DT_PHA_HAS_CELL_AT_IDX(TOUCHPAD_NODE_ID, bindings, idx, param1),     \
                              (0), (DT_PHA_BY_IDX(TOUCHPAD_NODE_ID, bindings, idx, param1))),      \
        .param2 = COND_CODE_0(DT_PHA_HAS_CELL_AT_IDX(TOUCHPAD_NODE_ID, bindings, idx, param2),     \
                              (0), (DT_PHA_BY_IDX(TOUCHPAD_NODE_ID, bindings, idx, param2))),      \
    }

static struct zmk_behavior_binding touchpad_bindings[MPR121_TOUCHPAD_NUM_BINDINGS] = {
    _TP_BIND(0), _TP_BIND(1), _TP_BIND(2), _TP_BIND(3),
    _TP_BIND(4), _TP_BIND(5), _TP_BIND(6), _TP_BIND(7)};

static void invoke_binding(uint8_t binding_idx) {
    if (binding_idx >= MPR121_TOUCHPAD_NUM_BINDINGS) {
        return;
    }

    struct zmk_behavior_binding *binding = &touchpad_bindings[binding_idx];
    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
    if (!dev) {
        LOG_ERR("Binding device not found: %s", binding->behavior_dev);
        return;
    }

    struct zmk_behavior_binding_event event = {
        .layer = 0,
        .position = 0,
        .timestamp = k_uptime_get(),
    };

#if IS_ENABLED(CONFIG_ZMK_SPLIT)
    event.source = 0;
#endif

    struct zmk_behavior_binding bind_copy = *binding;
    LOG_DBG("Invoke binding %d: %s p1=0x%02X p2=0x%02X", binding_idx, binding->behavior_dev,
            binding->param1, binding->param2);
    zmk_behavior_invoke_binding(&bind_copy, event, true);
    zmk_behavior_invoke_binding(&bind_copy, event, false);
}

static int mpr121_touchpad_handler_start(const zmk_event_t *eh) {
    struct zmk_mpr121_touch_start_event *evt = as_zmk_mpr121_touch_start_event(eh);
    if (!evt) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    LOG_INF("Touch start: x=%d y=%d", (int)(evt->x * 100), (int)(evt->y * 100));
    // invoke_binding(0);
    return ZMK_EV_EVENT_BUBBLE;
}

static int mpr121_touchpad_handler_move(const zmk_event_t *eh) {
    struct zmk_mpr121_touch_move_event *evt = as_zmk_mpr121_touch_move_event(eh);
    if (!evt) {
        return ZMK_EV_EVENT_BUBBLE;
    }

#if IS_ENABLED(CONFIG_ZMK_POINTING)
    float raw_dx = evt->dx;
    float raw_dy = evt->dy;
    zmk_hid_mouse_movement_set(raw_dx, raw_dy);
    zmk_endpoint_send_mouse_report();
    zmk_hid_mouse_movement_set(0, 0);
#endif

    return ZMK_EV_EVENT_BUBBLE;
}

static int mpr121_touchpad_handler_end(const zmk_event_t *eh) {
    struct zmk_mpr121_touch_end_event *evt = as_zmk_mpr121_touch_end_event(eh);
    if (!evt) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    LOG_INF("Touch end: x=%d y=%d dur=%u", (int)(evt->x * 100), (int)(evt->y * 100),
            evt->duration_ms);
    // invoke_binding(2);
    return ZMK_EV_EVENT_BUBBLE;
}

static int mpr121_touchpad_handler_tap(const zmk_event_t *eh) {
    struct zmk_mpr121_touch_tap_event *evt = as_zmk_mpr121_touch_tap_event(eh);
    if (!evt) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    LOG_INF("Tap: x=%d y=%d dur=%u", (int)(evt->x * 100), (int)(evt->y * 100), evt->duration_ms);
    // invoke_binding(3);
    return ZMK_EV_EVENT_BUBBLE;
}

static int mpr121_touchpad_handler_gesture(const zmk_event_t *eh) {
    struct zmk_mpr121_gesture_event *evt = as_zmk_mpr121_gesture_event(eh);
    if (!evt) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    static const char *names[] = {"NONE", "LEFT", "RIGHT", "UP", "DOWN"};
    LOG_INF("Gesture: %s disp=%u vel=%u", names[evt->gesture_type], evt->displacement,
            evt->velocity);

    uint8_t binding_idx = 4 + (evt->gesture_type - 1);
    // invoke_binding(binding_idx);
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(mpr121_touchpad_listener, mpr121_touchpad_handler_start);
ZMK_SUBSCRIPTION(mpr121_touchpad_listener, zmk_mpr121_touch_start_event);

ZMK_LISTENER(mpr121_touchpad_listener_move, mpr121_touchpad_handler_move);
ZMK_SUBSCRIPTION(mpr121_touchpad_listener_move, zmk_mpr121_touch_move_event);

ZMK_LISTENER(mpr121_touchpad_listener_end, mpr121_touchpad_handler_end);
ZMK_SUBSCRIPTION(mpr121_touchpad_listener_end, zmk_mpr121_touch_end_event);

ZMK_LISTENER(mpr121_touchpad_listener_tap, mpr121_touchpad_handler_tap);
ZMK_SUBSCRIPTION(mpr121_touchpad_listener_tap, zmk_mpr121_touch_tap_event);

ZMK_LISTENER(mpr121_touchpad_listener_gesture, mpr121_touchpad_handler_gesture);
ZMK_SUBSCRIPTION(mpr121_touchpad_listener_gesture, zmk_mpr121_gesture_event);

#endif
