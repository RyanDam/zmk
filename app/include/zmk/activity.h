/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

enum zmk_activity_state { ZMK_ACTIVITY_ACTIVE, ZMK_ACTIVITY_IDLE, ZMK_ACTIVITY_SLEEP };

struct zmk_activity_config {
    bool sleep_enabled;
    uint32_t idle_timeout_ms;
    uint32_t sleep_timeout_ms;
};

enum zmk_activity_state zmk_activity_get_state(void);

/**
 * @brief Get the current activity/sleep configuration.
 */
struct zmk_activity_config zmk_activity_get_config(void);

/**
 * @brief Apply a new activity/sleep configuration at runtime.
 *
 * Values are validated and persisted immediately. Applying a configuration also
 * resets the inactivity timer so the device does not immediately idle or sleep
 * because of time spent configuring it.
 *
 * @retval 0 on success.
 * @retval -EINVAL if the configuration is invalid.
 * @retval -ENOTSUP if sleep is requested but deep sleep is not compiled in.
 */
int zmk_activity_set_config(const struct zmk_activity_config *config);

/**
 * @brief Restore the compile-time default activity/sleep configuration.
 */
int zmk_activity_reset_config(void);
