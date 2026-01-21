/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(zmk_studio, CONFIG_ZMK_STUDIO_LOG_LEVEL);

#include <drivers/behavior.h>

#include <zmk/behavior.h>
#include <zmk/keymap.h>
#include <zmk/studio/rpc.h>

#include <pb_encode.h>

ZMK_RPC_SUBSYSTEM(sensors)

#define SENSOR_RESPONSE(type, ...) ZMK_RPC_RESPONSE(sensors, type, __VA_ARGS__)
#define SENSOR_NOTIFICATION(type, ...) ZMK_RPC_NOTIFICATION(sensors, type, __VA_ARGS__)
#define KEYMAP_NOTIFICATION(type, ...) ZMK_RPC_NOTIFICATION(keymap, type, __VA_ARGS__)



struct sensor_layer_state {
    uint8_t sensor_idx;
    uint8_t layer_idx;
};

static bool should_unpack_behavior(const char *behavior_name, zmk_behavior_local_id_t *child_bid) {
    // Handle both the standard name and common aliases/user names
    if (strcmp(behavior_name, "sensor_rotate_kp") == 0 || 
        strcmp(behavior_name, "inc_dec_kp") == 0) {
        
        *child_bid = zmk_behavior_get_local_id("kp");
        if (*child_bid == UINT16_MAX) {
            *child_bid = zmk_behavior_get_local_id("key_press");
        }
        return true;
    }
    return false;
}

static bool encode_layer_name(pb_ostream_t *stream, const pb_field_t *field, void *const *arg) {
    const zmk_keymap_layer_index_t layer_idx = *(uint8_t *)*arg;

    const char *name = zmk_keymap_layer_name(layer_idx);

    if (!name) {
        return true;
    }

    if (!pb_encode_tag_for_field(stream, field)) {
        return false;
    }

    return pb_encode_string(stream, name, strlen(name));
}

static bool encode_layer(pb_ostream_t *stream, const pb_field_t *field, void *const *arg) {
    struct sensor_layer_state *state = (struct sensor_layer_state *)*arg;
    zmk_keymap_layer_id_t layer_id = zmk_keymap_layer_index_to_id(state->layer_idx);

    if (layer_id == UINT8_MAX) {
        return true;
    }

    if (!pb_encode_tag_for_field(stream, field)) {
        return false;
    }

    zmk_sensors_Layer layer_msg = zmk_sensors_Layer_init_zero;
    layer_msg.id = layer_id;

    layer_msg.name.funcs.encode = encode_layer_name;
    layer_msg.name.arg = &state->layer_idx;

    layer_msg.bindings_count = CONFIG_ZMK_KEYMAP_SENSORS_MAX_BINDINGS;
    const struct zmk_behavior_binding *slot0 = zmk_keymap_get_layer_sensor_binding_at_idx(layer_id, state->sensor_idx, 0);
    const struct zmk_behavior_binding *slot1 = zmk_keymap_get_layer_sensor_binding_at_idx(layer_id, state->sensor_idx, 1);

    zmk_behavior_local_id_t child_bid = 0;
    if (slot0 && slot0->behavior_dev && (!slot1 || !slot1->behavior_dev) &&
        should_unpack_behavior(slot0->behavior_dev, &child_bid)) {
        
        // Unpack combined behavior into two virtual slots for Studio
        layer_msg.bindings[0].behavior_id = zmk_behavior_get_local_id(slot0->behavior_dev);
        layer_msg.bindings[0].param1 = slot0->param1;
        layer_msg.bindings[0].param2 = 0;

        layer_msg.bindings[1].behavior_id = zmk_behavior_get_local_id(slot0->behavior_dev);
        layer_msg.bindings[1].param1 = slot0->param2;
        layer_msg.bindings[1].param2 = 0;
    } else {
        for (int i = 0; i < CONFIG_ZMK_KEYMAP_SENSORS_MAX_BINDINGS; i++) {
            const struct zmk_behavior_binding *binding = (i == 0) ? slot0 : slot1;

            if (binding && binding->behavior_dev) {
                layer_msg.bindings[i].behavior_id = zmk_behavior_get_local_id(binding->behavior_dev);
                layer_msg.bindings[i].param1 = binding->param1;
                layer_msg.bindings[i].param2 = binding->param2;
            }
        }
    }

    return pb_encode_submessage(stream, &zmk_sensors_Layer_msg, &layer_msg);
}

static bool encode_layers(pb_ostream_t *stream, const pb_field_t *field, void *const *arg) {
    uint8_t sensor_idx = *(uint8_t *)*arg;

    for (zmk_keymap_layer_index_t i = 0; i < ZMK_KEYMAP_LAYERS_LEN; i++) {
        struct sensor_layer_state state = {.sensor_idx = sensor_idx, .layer_idx = i};
        struct sensor_layer_state *state_ptr = &state;
        if (!encode_layer(stream, field, (void *const *)&state_ptr)) {
            return false;
        }
    }
    return true;
}

static bool encode_sensor_list(pb_ostream_t *stream, const pb_field_t *field, void *const *arg) {
    for (int i = 0; i < ZMK_KEYMAP_SENSORS_LEN; i++) {
        if (!pb_encode_tag_for_field(stream, field)) {
            return false;
        }
        if (!pb_encode_varint(stream, i)) {
            return false;
        }
    }
    return true;
}

zmk_studio_Response list_all_sensors(const zmk_studio_Request *req) {
    LOG_DBG("");
    zmk_sensors_ListAllSensorsResponse resp = zmk_sensors_ListAllSensorsResponse_init_zero;
    resp.sensors.funcs.encode = encode_sensor_list;

    return SENSOR_RESPONSE(list_all_sensors, resp);
}

static bool encode_sensor_name(pb_ostream_t *stream, const pb_field_t *field, void *const *arg) {
    if (!pb_encode_tag_for_field(stream, field)) {
        return false;
    }

    return pb_encode_string(stream, "", 0);
}

zmk_studio_Response get_sensor_details(const zmk_studio_Request *req) {
    LOG_DBG("");
    const zmk_sensors_GetSensorDetailsRequest *get_req =
        &req->subsystem.sensors.request_type.get_sensor_details;

    if (get_req->sensor_id >= ZMK_KEYMAP_SENSORS_LEN) {
        return ZMK_RPC_SIMPLE_ERR(GENERIC);
    }

    // Use static to keep valid for pb encoding
    static uint8_t sensor_idx;
    sensor_idx = get_req->sensor_id;

    zmk_sensors_GetSensorDetailsResponse resp = zmk_sensors_GetSensorDetailsResponse_init_zero;
    resp.sensor_id = sensor_idx;
    resp.display_name.funcs.encode = encode_sensor_name;

    resp.layers.funcs.encode = encode_layers;
    resp.layers.arg = &sensor_idx;

    return SENSOR_RESPONSE(get_sensor_details, resp);
}

zmk_studio_Response set_sensor_details(const zmk_studio_Request *req) {
    LOG_DBG("");
    const zmk_sensors_SetSensorDetailsRequest *set_req =
        &req->subsystem.sensors.request_type.set_sensor_details;

    if (set_req->sensor_id >= ZMK_KEYMAP_SENSORS_LEN) {
        return SENSOR_RESPONSE(
            set_sensor_details,
            zmk_sensors_SetSensorDetailsResponse_SET_SENSOR_DETAILS_RESP_INVALID_SENSOR);
    }

    for (int i = 0; i < CONFIG_ZMK_KEYMAP_SENSORS_MAX_BINDINGS; i++) {
        struct zmk_behavior_binding binding = {0};

        if (i < set_req->bindings_count) {
            const zmk_sensors_BehaviorBinding *req_binding = &set_req->bindings[i];

            zmk_behavior_local_id_t bid = req_binding->behavior_id;
            const char *behavior_name = zmk_behavior_find_behavior_name_from_local_id(bid);

            if (!behavior_name) {
                return SENSOR_RESPONSE(
                    set_sensor_details,
                    zmk_sensors_SetSensorDetailsResponse_SET_SENSOR_DETAILS_RESP_INVALID_BEHAVIOR);
            }

            binding = (struct zmk_behavior_binding){
                .behavior_dev = behavior_name,
                .param1 = req_binding->param1,
                .param2 = req_binding->param2,
            };

            // int ret = zmk_behavior_validate_binding(&binding);
            // if (ret < 0) {
            //     return SENSOR_RESPONSE(
            //         set_sensor_details,
            //         zmk_sensors_SetSensorDetailsResponse_SET_SENSOR_DETAILS_RESP_INVALID_PARAMETERS);
            // }
        }

        int ret = zmk_keymap_set_layer_sensor_binding_at_idx(set_req->layer_id, set_req->sensor_id, i,
                                                             binding);

        if (ret < 0) {
            LOG_WRN("Setting the binding failed with %d", ret);
            switch (ret) {
            case -EINVAL:
                return SENSOR_RESPONSE(
                    set_sensor_details,
                    zmk_sensors_SetSensorDetailsResponse_SET_SENSOR_DETAILS_RESP_INVALID_LAYER);
            default:
                return ZMK_RPC_SIMPLE_ERR(GENERIC);
            }
        }
    }

    raise_zmk_studio_rpc_notification((struct zmk_studio_rpc_notification){
        .notification = KEYMAP_NOTIFICATION(unsaved_changes_status_changed, true)});

    return SENSOR_RESPONSE(set_sensor_details,
                           zmk_sensors_SetSensorDetailsResponse_SET_SENSOR_DETAILS_RESP_OK);
}

ZMK_RPC_SUBSYSTEM_HANDLER(sensors, list_all_sensors, ZMK_STUDIO_RPC_HANDLER_UNSECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(sensors, get_sensor_details, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(sensors, set_sensor_details, ZMK_STUDIO_RPC_HANDLER_SECURED);
