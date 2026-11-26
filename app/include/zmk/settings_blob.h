/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Shared skeleton for the "one settings blob per tunable" pattern: key
 * match + size validation + load/save; field validation stays with the
 * owner. Include AFTER the TU's LOG_MODULE_REGISTER/DECLARE so logs land
 * in the caller's module.
 */

#pragma once

#include <string.h>

#include <zephyr/settings/settings.h>
#include <zephyr/logging/log.h>

/** @returns 1 when @p dst holds the loaded blob, 0 for a foreign key (pass
 *  through), negative errno on size/read failure. */
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

/** Save a blob; persistence is best-effort (warn on failure, live value
 *  still applies). */
static inline void zmk_settings_blob_save(const char *key, const void *blob, size_t size,
                                          const char *what) {
    int err = settings_save_one(key, blob, size);

    if (err) {
        LOG_WRN("Could not persist %s (err %d) - live apply only", what, err);
    }
}
