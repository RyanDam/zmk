/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

/* Live keymap table changed; `structural` = layer order/names may differ too. */

#pragma once

#include <zephyr/kernel.h>
#include <zmk/event_manager.h>

struct zmk_keymap_changed {
    bool structural;
};

ZMK_EVENT_DECLARE(zmk_keymap_changed);
