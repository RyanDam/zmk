/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Shared skeleton for the "one settings blob per tunable" pattern used by
 * the tab1a subsystems (theme, home screen, clock format, stats,
 * pomodoro, anim bg): key match + size validation + read on load, save with
 * a uniform warning on failure. Field/value validation stays with the
 * owner — only it knows what a loaded value means.
 *
 * Include this AFTER the translation unit's LOG_MODULE_REGISTER/DECLARE so
 * the log lines land in the caller's module.
 */

#pragma once

#include <string.h>

#include <zephyr/settings/settings.h>
#include <zephyr/logging/log.h>

/**
 * @brief Match the key and read a same-size blob out of a settings handler.
 *
 * @returns 1 when @p dst holds the loaded blob (caller validates fields and
 * applies), 0 when the key belongs to another handler under the same
 * subtree (return 0 from the handler in that case), negative errno on a
 * size mismatch or read failure.
 */
static inline int zmk_settings_blob_read(const char *key, size_t len, settings_read_cb read_cb,
                                         void *cb_arg, const char *wanted_key, void *dst,
                                         size_t dst_size, const char *what) {
    int err;

    if (strcmp(wanted_key, key) != 0) {
        return 0;
    }
    if (len != dst_size) {
        LOG_WRN("bad %s settings size %zu (want %zu)", what, len, dst_size);
        return -EINVAL;
    }

    err = read_cb(cb_arg, dst, dst_size);
    if (err <= 0) {
        LOG_ERR("Failed to read %s settings (err %d)", what, err);
        return err < 0 ? err : -EINVAL;
    }
    return 1;
}

/** Save a blob, warning-and-continuing: persistence is best-effort, the
 *  in-RAM value still applies live (c02 gated anyway). */
static inline void zmk_settings_blob_save(const char *key, const void *blob, size_t size,
                                          const char *what) {
    int err = settings_save_one(key, blob, size);

    if (err) {
        LOG_WRN("Could not persist %s (err %d) - live apply only", what, err);
    }
}
