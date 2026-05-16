
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/logging/log.h>
#include <zephyr/kernel.h>

#include <zmk/behavior_queue.h>
#include <zmk/virtual_key_position.h>
#include <zmk/events/position_state_changed.h>

#include "behavior_sensor_rotate_common.h"
#include "zmk/behavior.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

int zmk_behavior_sensor_rotate_common_accept_data(
    struct zmk_behavior_binding *binding, struct zmk_behavior_binding_event event,
    const struct zmk_sensor_config *sensor_config, size_t channel_data_size,
    const struct zmk_sensor_channel_data *channel_data) {
    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
    struct behavior_sensor_rotate_data *data = dev->data;

    const struct sensor_value value = channel_data[0].value;
    int triggers;
    int sensor_index = ZMK_SENSOR_POSITION_FROM_VIRTUAL_KEY_POSITION(event.position);

    // Some funky special casing for "old encoder behavior" where ticks where reported in val2 only,
    // instead of rotational degrees in val1.
    // REMOVE ME: Remove after a grace period of old ec11 sensor behavior
    if (value.val1 == 0) {
        triggers = value.val2;
    } else {
        struct sensor_value remainder = data->remainder[sensor_index][event.layer];

        remainder.val1 += value.val1;
        remainder.val2 += value.val2;

        if (abs(remainder.val2) >= 1000000) {
            remainder.val1 += remainder.val2 / 1000000;
            remainder.val2 %= 1000000;
        }

        int trigger_degrees = 360 / sensor_config->triggers_per_rotation;
        triggers = remainder.val1 / trigger_degrees;
        remainder.val1 %= trigger_degrees;

        data->remainder[sensor_index][event.layer] = remainder;
    }

    LOG_DBG(
        "val1: %d, val2: %d, remainder: %d/%d triggers: %d inc keycode 0x%02X dec keycode 0x%02X",
        value.val1, value.val2, data->remainder[sensor_index][event.layer].val1,
        data->remainder[sensor_index][event.layer].val2, triggers, binding->param1,
        binding->param2);

    data->triggers[sensor_index][event.layer] = triggers;
    return 0;
}

int zmk_behavior_sensor_rotate_common_process(struct zmk_behavior_binding *binding,
                                              struct zmk_behavior_binding_event event,
                                              enum behavior_sensor_binding_process_mode mode) {
    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
    const struct behavior_sensor_rotate_config *cfg = dev->config;
    struct behavior_sensor_rotate_data *data = dev->data;

    const int sensor_index = ZMK_SENSOR_POSITION_FROM_VIRTUAL_KEY_POSITION(event.position);

    if (mode != BEHAVIOR_SENSOR_BINDING_PROCESS_MODE_TRIGGER) {
        data->triggers[sensor_index][event.layer] = 0;
        return ZMK_BEHAVIOR_TRANSPARENT;
    }

    int triggers = data->triggers[sensor_index][event.layer];

    // LOG_DBG("dt_binding dev: %s, param1: %d, param2: %d", binding->behavior_dev, binding->param1,
    //         binding->param2);

    struct zmk_behavior_binding triggered_binding;
    if (triggers != 0) {
        // Clarify on how the binding work
        // - For freshly loaded firmware, the keymap_binding will have param 1 and param 2 is for cw
        // and ccw example: dt_binding dev: sensor_rotate_kp, param1: 458827, param2: 458830. extra
        // note is for ccw binding, the keymap binding will be empty: ccw_binding dev: , param1: 0,
        // param2: 0
        // - For studio loaded binding, the keymap_binding will have param 1 is for binding and
        // param 2 is for behavior local id.

        const struct zmk_behavior_binding *cw_binding =
            zmk_keymap_get_layer_sensor_binding_at_idx(event.layer, sensor_index, 0);
        const struct zmk_behavior_binding *ccw_binding =
            zmk_keymap_get_layer_sensor_binding_at_idx(event.layer, sensor_index, 1);

        // LOG_DBG("cw_binding dev: %s, param1: %d, param2: %d", cw_binding->behavior_dev,
        //         cw_binding->param1, cw_binding->param2);
        // LOG_DBG("ccw_binding dev: %s, param1: %d, param2: %d", ccw_binding->behavior_dev,
        //         ccw_binding->param1, ccw_binding->param2);

        if (ccw_binding->param1 == 0) {
            // ccw_binding is empty, which means this is raw binding from device tree
            // need to extract cw and ccw from dt_binding
            const uint32_t binding_id = triggers > 0 ? binding->param1 : binding->param2;
            const zmk_behavior_local_id_t behavior_local_id =
                zmk_behavior_get_local_id("key_press");

            // LOG_DBG("dt binding id: %d, local id: %d", binding_id, behavior_local_id);

            triggered_binding.behavior_dev =
                zmk_behavior_find_behavior_name_from_local_id(behavior_local_id);
            triggered_binding.param1 = binding_id;
            triggered_binding.param2 = 0;
        } else {
            // keymap binding is set from the studio code
            const uint32_t binding_id = triggers > 0 ? cw_binding->param1 : ccw_binding->param1;
            zmk_behavior_local_id_t behavior_local_id =
                triggers > 0 ? cw_binding->param2 : ccw_binding->param2;

            // LOG_DBG("keymap binding id: %d, local id: %d", binding_id, behavior_local_id);

            if (behavior_local_id == 0) {
                // hardcode to key_press
                behavior_local_id = zmk_behavior_get_local_id("key_press");
                LOG_DBG("keymap fallback local id: %d", behavior_local_id);
            }
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_LOCAL_IDS_IN_BINDINGS)
            triggered_binding.local_id = behavior_local_id;
#endif // IS_ENABLED(CONFIG_ZMK_BEHAVIOR_LOCAL_IDS_IN_BINDINGS)
            triggered_binding.behavior_dev =
                zmk_behavior_find_behavior_name_from_local_id(behavior_local_id);
            triggered_binding.param1 = binding_id;
            triggered_binding.param2 = 0;
        }
    } else {
        return ZMK_BEHAVIOR_TRANSPARENT;
    }

    // LOG_DBG("triggered_binding dev: %s, param1: %d, param2: %d", triggered_binding.behavior_dev,
    //         triggered_binding.param1, triggered_binding.param2);

#if IS_ENABLED(CONFIG_ZMK_SPLIT)
    // set this value so that it always triggers on central, can be handled more properly later
    event.source = ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL;
#endif

    triggers = abs(triggers);
    for (int i = 0; i < triggers; i++) {
        // LOG_DBG("Sensor tap time ms: %d", cfg->tap_ms);
        zmk_behavior_queue_add(&event, triggered_binding, true, 60);
        zmk_behavior_queue_add(&event, triggered_binding, false, 0);
    }

    return ZMK_BEHAVIOR_OPAQUE;
}
