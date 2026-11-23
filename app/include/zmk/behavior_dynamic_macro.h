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

#define ZMK_DYNAMIC_MACRO_NAME_MAX 16

/** Sentinel for "every macro changed" (settings reload, discard). */
#define ZMK_DYNAMIC_MACRO_ALL UINT32_MAX

int zmk_dynamic_macro_get_count(void);
int zmk_dynamic_macro_get_max_steps(void);
int zmk_dynamic_macro_get_step_length(uint32_t macro_idx);
int zmk_dynamic_macro_set_step_length(uint32_t macro_idx, uint8_t length);
struct zmk_dynamic_macro_step *zmk_dynamic_macro_get_step(uint32_t macro_idx, uint32_t step_idx);
int zmk_dynamic_macro_set_step(uint32_t macro_idx, uint32_t step_idx,
                               struct zmk_dynamic_macro_step step);

/**
 * Macro display names (pad10a eeprom/cb_eeprom set_macro_name spirit): empty
 * by default; UIs fall back to their own label (e.g. "M<idx+1>"). Staged like
 * steps: set marks unsaved, zmk_dynamic_macro_save_changes() persists, and
 * discard/Settings-load revert.
 */
const char *zmk_dynamic_macro_get_name(uint32_t macro_idx);
int zmk_dynamic_macro_set_name(uint32_t macro_idx, const char *name);

/** Change callback (any thread; macro_idx == ZMK_DYNAMIC_MACRO_ALL for all). */
typedef void (*zmk_dynamic_macro_changed_cb)(uint32_t macro_idx);
int zmk_dynamic_macro_add_listener(zmk_dynamic_macro_changed_cb cb);

int zmk_dynamic_macro_save_changes(void);
int zmk_dynamic_macro_discard_changes(void);
int zmk_dynamic_macro_check_unsaved_changes(void);
