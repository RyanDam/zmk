/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * @file
 * Recovery hooks shared by the AXS15231B display and touch drivers.
 *
 * Bench 2026-11-21/22: while the panel streams pixel frames over QSPI, the
 * touch engine inside the AXS15231B intermittently latches into a stuck
 * mailbox state — every I2C reply becomes the same frozen frame (all 0x02,
 * all 0x41, all 0x45, counter byte included), and only a controller reset
 * clears it. A dark panel (no pixel traffic) never latches it; touches
 * stream cleanly at 50 Hz with the frame counter ticking. With animation
 * frames (8-15 fps full-refresh) the latch became constant.
 *
 * So the drivers treat it as a recoverable hardware fault: the touch
 * watchdog detects a frozen reply stream and asks the display driver to
 * reset and re-initialise the chip (SLPIN -> SWRESET -> vendor init table
 * -> DISPON); on success the app layer repaints the whole UI, because the
 * panel's GRAM is black after re-init while LVGL still believes the screen
 * is drawn.
 */

#ifndef ZMK_AXS15231B_RECOVERY_H
#define ZMK_AXS15231B_RECOVERY_H

#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif

#if IS_ENABLED(CONFIG_AXS15231B)

/**
 * @brief Reset and re-initialise an AXS15231B panel.
 *
 * Safe to call from a work item (it sleeps for the reset/sleep-out delays).
 * Leaves the GRAM black: the caller must repaint, e.g.
 * lv_obj_report_style_change(NULL) with the LVGL lock held.
 *
 * @param dev the display device (AXS15231B instance)
 * @param hard escalate: SLPIN first in addition to SWRESET (use after a
 *        soft recovery failed to un-stick the chip)
 * @return 0 on success, negative errno otherwise
 */
int zmk_axs15231b_recover(const struct device *dev, bool hard);

#endif /* CONFIG_AXS15231B */

#if IS_ENABLED(CONFIG_AXS15231B_TOUCH)

/**
 * @brief Snapshot of the touch controller's health (for the bench page).
 *
 * @param frozen    true while the mailbox-freeze watchdog counts identical replies
 * @param same      identical implausible replies counted so far
 * @param stale     presses the freshness timer had to force-release
 * @param recoveries recoveries performed since boot
 * @param frame     last raw 8-byte reply (may be NULL)
 */
void zmk_axs15231b_touch_status(bool *frozen, unsigned int *same, unsigned int *stale,
                                unsigned int *recoveries, uint8_t frame[8]);

#endif /* CONFIG_AXS15231B_TOUCH */

#ifdef __cplusplus
}
#endif

#endif /* ZMK_AXS15231B_RECOVERY_H */
