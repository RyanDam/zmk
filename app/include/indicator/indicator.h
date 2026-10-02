#include <stdbool.h>

#include <zephyr/toolchain.h>

/* indicate_connectivity() and indicate_layer() depend on central-side state
 * (BLE profiles, keymap layers). On the peripheral half of a split build
 * those symbols do not exist, so the corresponding code is compiled out. */
#define INDICATOR_CENTRAL_ONLY                                                                     \
    (!IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL))

#if IS_ENABLED(CONFIG_ZMK_BLE) && INDICATOR_CENTRAL_ONLY
void indicate_connectivity(void);
#endif

#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)
void indicate_battery(void);
#endif

#if INDICATOR_CENTRAL_ONLY
void indicate_layer(void);
#endif

#if IS_ENABLED(CONFIG_MPR121)
void indicate_touchpad_irq(bool active);
#endif