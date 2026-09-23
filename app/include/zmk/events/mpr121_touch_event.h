/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <zephyr/kernel.h>
#include <zmk/event_manager.h>
#include <stdint.h>

struct zmk_mpr121_touch_start_event {
    float x;
    float y;
};

ZMK_EVENT_DECLARE(zmk_mpr121_touch_start_event);

struct zmk_mpr121_touch_move_event {
    float dx;
    float dy;
    float x;
    float y;
};

ZMK_EVENT_DECLARE(zmk_mpr121_touch_move_event);

struct zmk_mpr121_touch_end_event {
    float x;
    float y;
    uint32_t duration_ms;
};

ZMK_EVENT_DECLARE(zmk_mpr121_touch_end_event);

struct zmk_mpr121_touch_tap_event {
    float x;
    float y;
    uint32_t duration_ms;
};

ZMK_EVENT_DECLARE(zmk_mpr121_touch_tap_event);
