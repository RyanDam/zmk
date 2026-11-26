/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * @file
 * Recovery hooks shared by the AXS15231B display and touch drivers.
 *
 * While the panel streams pixel frames over QSPI, the touch engine inside the
 * chip latches into a stuck mailbox state: every I2C reply becomes one frozen
 * frame and only a controller reset clears it. So the touch watchdog detects a
 * frozen stream and asks the display driver to reset + re-init the chip; the app
 * must then repaint, because the GRAM is black after re-init while LVGL believes
 * it is drawn.
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
 * Safe from a work item (it sleeps through the reset/sleep-out delays). Leaves
 * the GRAM black: the caller must repaint, e.g. lv_obj_report_style_change(NULL)
 * with the LVGL lock held.
 *
 * @param dev  the display device
 * @param hard escalate with SLPIN before SWRESET, after a soft reset failed
 * @return 0 on success, negative errno otherwise
 */
int zmk_axs15231b_recover(const struct device *dev, bool hard);

#endif /* CONFIG_AXS15231B */

#if IS_ENABLED(CONFIG_AXS15231B_TOUCH)

/**
 * @brief Touch-controller health snapshot for the bench page.
 *
 * @param frozen     true while the watchdog counts identical replies
 * @param same       identical implausible replies counted so far
 * @param stale      presses the freshness timer force-released
 * @param recoveries recoveries since boot
 * @param frame      last raw 8-byte reply (may be NULL)
 */
void zmk_axs15231b_touch_status(bool *frozen, unsigned int *same, unsigned int *stale,
                                unsigned int *recoveries, uint8_t frame[8]);

#endif /* CONFIG_AXS15231B_TOUCH */

#ifdef __cplusplus
}
#endif

#endif /* ZMK_AXS15231B_RECOVERY_H */
