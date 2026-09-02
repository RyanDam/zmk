#include <stdbool.h>

#if IS_ENABLED(CONFIG_ZMK_BLE)
void indicate_connectivity(void);
#endif

#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)
void indicate_battery(void);
#endif

void indicate_layer(void);

#if IS_ENABLED(CONFIG_MPR121)
void indicate_touchpad_irq(bool active);
#endif