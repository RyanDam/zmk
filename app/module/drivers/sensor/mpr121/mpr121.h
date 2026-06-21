/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <stdint.h>

#define MPR121_I2C_ADDRESS 0x5A

#define MPR121_TOUCHSTATUS_L 0x00
#define MPR121_TOUCHSTATUS_H 0x01
#define MPR121_FILTDATA_0 0x04
#define MPR121_BASELINE_0 0x1E
#define MPR121_MHDR 0x2B
#define MPR121_NHDR 0x2C
#define MPR121_NCLR 0x2D
#define MPR121_FDLR 0x2E
#define MPR121_MHDF 0x2F
#define MPR121_NHDF 0x30
#define MPR121_NCLF 0x31
#define MPR121_FDLF 0x32
#define MPR121_NHDT 0x33
#define MPR121_NCLT 0x34
#define MPR121_FDLT 0x35
#define MPR121_TOUCHTH_0 0x41
#define MPR121_RELEASETH_0 0x42
#define MPR121_DEBOUNCE 0x5B
#define MPR121_CONFIG1 0x5C
#define MPR121_CONFIG2 0x5D
#define MPR121_ECR 0x5E
#define MPR121_SOFTRESET 0x80

#define MPR121_NUM_ELECTRODES 12
#define MPR121_NUM_COLS 6
#define MPR121_NUM_ROWS 6

enum mpr121_gesture_type {
    MPR121_GESTURE_NONE = 0,
    MPR121_GESTURE_SWIPE_LEFT,
    MPR121_GESTURE_SWIPE_RIGHT,
    MPR121_GESTURE_SWIPE_UP,
    MPR121_GESTURE_SWIPE_DOWN,
};

struct mpr121_grid_pos {
    float x;
    float y;
};

struct mpr121_config {
    struct i2c_dt_spec i2c;
    const struct gpio_dt_spec interrupt_gpio;
    uint8_t touch_threshold;
    uint8_t release_threshold;
    uint16_t gesture_min_displacement;
    uint16_t gesture_min_velocity;
    uint32_t gesture_max_duration_ms;
    uint32_t poll_interval_ms;
    uint32_t tap_max_duration_ms;
    uint16_t tap_max_displacement;
    uint16_t movement_scale;
};

struct mpr121_data {
    const struct device *dev;
    const struct mpr121_config *config;
    struct gpio_callback gpio_cb;
    struct k_work_delayable poll_work;

    uint16_t last_touch_status;
    bool is_touched;
    struct mpr121_grid_pos start_pos;
    struct mpr121_grid_pos last_pos;
    uint32_t touch_start_time;
    float total_movement;
};
