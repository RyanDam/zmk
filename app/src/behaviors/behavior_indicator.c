#define DT_DRV_COMPAT zmk_behavior_indicator

#include <zephyr/device.h>
#include <drivers/behavior.h>
#include <zephyr/logging/log.h>

#include <indicator/indicator.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

struct behavior_indicator_config {
    bool indicate_battery;
    bool indicate_layer;
};

static int behavior_indicator_init(const struct device *dev) { return 0; }

static int on_keymap_binding_pressed(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event) {

    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
    const struct behavior_indicator_config *cfg = dev->config;

#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)
    if (cfg->indicate_battery) {
        indicate_battery();
    }
#endif

    if (cfg->indicate_layer) {
        indicate_layer();
    }

    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_keymap_binding_released(struct zmk_behavior_binding *binding,
                                      struct zmk_behavior_binding_event event) {
    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api behavior_indicator_driver_api = {
    .binding_pressed = on_keymap_binding_pressed,
    .binding_released = on_keymap_binding_released,
    .locality = BEHAVIOR_LOCALITY_GLOBAL,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .get_parameter_metadata = zmk_behavior_get_empty_param_metadata,
#endif // IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
};

#define IND_INST(n)                                                                                \
    static struct behavior_indicator_config behavior_indicator_config_##n = {                      \
        .indicate_battery = DT_INST_PROP(n, indicate_battery),                                     \
        .indicate_layer = DT_INST_PROP(n, indicate_layer),                                         \
    };                                                                                             \
    BEHAVIOR_DT_INST_DEFINE(n, behavior_indicator_init, NULL, NULL, &behavior_indicator_config_##n,  \
                            POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,                      \
                            &behavior_indicator_driver_api);

DT_INST_FOREACH_STATUS_OKAY(IND_INST)
