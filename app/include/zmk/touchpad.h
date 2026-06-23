/*
 * Copyright (c) 2024 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <zmk/behavior.h>
#include <zmk/keymap.h>

#define TOUCHPAD_NUM_BINDINGS 7

typedef enum {
    TOUCHPAD_MODE_GESTURE = 0,
    TOUCHPAD_MODE_MOUSE_SIMULATION = 1,
} touchpad_mode_t;

typedef enum {
    TP_BIND_TOUCH_START = 0,
    TP_BIND_TOUCH_END = 1,
    TP_BIND_TAP = 2,
    TP_BIND_GESTURE_LEFT = 3,
    TP_BIND_GESTURE_RIGHT = 4,
    TP_BIND_GESTURE_UP = 5,
    TP_BIND_GESTURE_DOWN = 6,
} touchpad_binding_type_t;

touchpad_mode_t zmk_touchpad_get_mode(zmk_keymap_layer_id_t layer);
int zmk_touchpad_set_mode(zmk_keymap_layer_id_t layer, touchpad_mode_t mode);

const struct zmk_behavior_binding *zmk_touchpad_get_binding(zmk_keymap_layer_id_t layer,
                                                            touchpad_binding_type_t type);
int zmk_touchpad_set_binding(zmk_keymap_layer_id_t layer, touchpad_binding_type_t type,
                             struct zmk_behavior_binding binding);

int zmk_touchpad_check_unsaved_changes(void);
int zmk_touchpad_save_changes(void);
int zmk_touchpad_discard_changes(void);
int zmk_touchpad_reset_settings(void);
