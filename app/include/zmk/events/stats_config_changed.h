/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

/* Raised when the stats slot config (layout + 4 metric ids) changes. */

#pragma once

#include <zephyr/kernel.h>
#include <zmk/event_manager.h>

struct zmk_stats_config_changed {
    bool layout_changed; /* false = only slot metrics changed */
};

ZMK_EVENT_DECLARE(zmk_stats_config_changed);
