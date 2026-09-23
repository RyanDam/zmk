/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <zephyr/kernel.h>
#include <zmk/event_manager.h>

#include <mpr121.h>

struct zmk_mpr121_gesture_event {
    uint8_t gesture_type;
    uint16_t displacement;
    uint16_t velocity;
    uint32_t duration_ms;
};

ZMK_EVENT_DECLARE(zmk_mpr121_gesture_event);
