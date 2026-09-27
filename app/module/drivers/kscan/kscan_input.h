/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <zephyr/device.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>

/**
 * Report a matrix key press/release as one synchronized input event group:
 * INPUT_ABS_X (column), INPUT_ABS_Y (row), INPUT_BTN_TOUCH, then sync.
 *
 * Consumers (e.g. physical layouts) reassemble the row/column/state triplet
 * from the event group and act on the sync boundary.
 */
static inline void zmk_kscan_input_report(const struct device *dev, uint32_t row, uint32_t column,
                                          bool pressed) {
    input_report_abs(dev, INPUT_ABS_X, column, false, K_NO_WAIT);
    input_report_abs(dev, INPUT_ABS_Y, row, false, K_NO_WAIT);
    input_report_key(dev, INPUT_BTN_TOUCH, pressed, true, K_NO_WAIT);
}
