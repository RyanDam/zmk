/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <zephyr/kernel.h>
#include <zmk/behavior.h>

enum zmk_dynamic_macro_mode {
    MACRO_MODE_TAP = 0,
    MACRO_MODE_PRESS = 1,
    MACRO_MODE_RELEASE = 2,
};

struct zmk_dynamic_macro_step {
    struct zmk_behavior_binding binding;
    zmk_behavior_local_id_t behavior_local_id;
    uint32_t wait_ms;
    uint8_t mode;
};

int zmk_dynamic_macro_get_count(void);
int zmk_dynamic_macro_get_max_steps(void);
int zmk_dynamic_macro_get_step_length(uint32_t macro_idx);
int zmk_dynamic_macro_set_step_length(uint32_t macro_idx, uint8_t length);
struct zmk_dynamic_macro_step *zmk_dynamic_macro_get_step(uint32_t macro_idx, uint32_t step_idx);
int zmk_dynamic_macro_set_step(uint32_t macro_idx, uint32_t step_idx, struct zmk_dynamic_macro_step step);

int zmk_dynamic_macro_save_changes(void);
int zmk_dynamic_macro_discard_changes(void);
int zmk_dynamic_macro_check_unsaved_changes(void);
