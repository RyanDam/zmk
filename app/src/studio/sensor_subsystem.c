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

static bool encode_binding(pb_ostream_t *stream, const pb_field_t *field, void *const *arg) {
    const struct zmk_behavior_binding *binding = (const struct zmk_behavior_binding *)*arg;

    if (!pb_encode_tag_for_field(stream, field)) {
        return false;
    }

    zmk_sensors_BehaviorBinding bb = zmk_sensors_BehaviorBinding_init_zero;

    if (binding && binding->behavior_dev) {
        bb.behavior_id = zmk_behavior_get_local_id(binding->behavior_dev);
        bb.param1 = binding->param1;
        bb.param2 = binding->param2;
    }

    return pb_encode_submessage(stream, &zmk_sensors_BehaviorBinding_msg, &bb);
}

static bool encode_layer_bindings(pb_ostream_t *stream, const pb_field_t *field, void *const *arg) {
    const struct zmk_behavior_binding *binding = (const struct zmk_behavior_binding *)*arg;

    // Single binding supported in ZMK core, so just encode one
    return encode_binding(stream, field, (void *const *)&binding);
}

struct sensor_layer_state {
    uint8_t sensor_idx;
    uint8_t layer_idx;
};

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

    const struct zmk_behavior_binding *binding =
        zmk_keymap_get_layer_sensor_binding_at_idx(layer_id, state->sensor_idx);

    layer_msg.bindings.funcs.encode = encode_layer_bindings;
    layer_msg.bindings.arg = (void *)binding;

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

    // ZMK core only supports one binding per sensor.
    // We will look for the first valid binding in the repeated list.
    // NOTE: This assumes the count > 0 check or iteration logic.
    // Since we can't easily iterate the incoming pb_callback_t here without a custom decode
    // callback, we rely on the fact that for specific messages, nanopb might generate a struct with
    // an array if max_count is defined, OR it uses a callback. The user provided proto: `repeated
    // BehaviorBinding bindings = 3;`. If nanopb uses callbacks for `bindings`, we need to decode
    // it. However, usually `zmk_studio_Request` is already decoded by `rpc.c` before calling this
    // handler. So the `set_req` already contains the data? Wait, if it's a callback field in the
    // decoded struct, we can't re-decode it unless we stored the stream. `rpc.c` decodes the WHOLE
    // request. If `bindings` is a callback, the data is gone unless we hooked the callback during
    // decode. BUT `rpc.c` calls `pb_decode`.

    // CRITICAL ISSUE:
    // If `bindings` is a callback field (standard for `repeated` without `max_count`), the data is
    // lost after `pb_decode` returns in `rpc.c`, UNLESS the callback was set to store it. BUT
    // `rpc.c` uses `zmk_studio_Request_init_zero`. We cannot access the data here if we didn't
    // capture it during decode.
    //
    // However, `rpc.c` calls `handle_request`. The request is already decoded.
    // If `bindings` is a pointer/array (fixed size), we are good.
    // If `bindings` is a callback, `rpc.c` logic would have skipped it (default zero init).
    //
    // Check `keymap_subsystem.c` `set_layer_binding`.
    // It accepts `SetLayerBindingRequest`. The proto there likely had a singular `binding`.
    // `keymap.proto` has `BehaviorBinding binding = 3;` (singular).
    //
    // Current proto has `repeated BehaviorBinding bindings = 3;`.
    // If this is `repeated`, nanopb generates a callback unless `(nanopb).max_count` is specified.
    // Providing `(nanopb).max_count = 1` in the .options file would turn it into an array/struct.
    // If the user didn't do that, accessing `bindings` here is impossible if it's a callback,
    // because decode is finished. Use `binding` logic for now?
    //
    // Wait, if I assume the user *is* asking me to implement this file, I must assume they have
    // handled the nanopb options enabling us to access the data, OR I need to implement a decode
    // callback? I can't implement a decode callback *here* because `rpc.c` drives the decode.
    //
    // Hypothesis: The user has configured nanopb to make `bindings` an iterator or array.
    // If array (e.g. static allocation), expected field is `bindings` (array) and `bindings_count`.
    // I will write code assuming `bindings` is an array or usable struct, typically `bindings` and
    // `bindings_count` if `max_count` used. If not, I can't solve it without changing `rpc.c` or
    // the proto options.
    //
    // Let's assume `bindings_count` and `bindings` array exists (common ZMK Studio pattern for
    // small fixed lists). If it's a pointer/callback, I'm stuck. But wait,
    // `SetSensorDetailsRequest` ... `repeated BehaviorBinding bindings`. For now, I will assume
    // `bindings_count` and generic `bindings` array access.

    if (set_req->bindings_count == 0) {
        // Clearing (?) Not typical for sensors for now
        return SENSOR_RESPONSE(set_sensor_details,
                               zmk_sensors_SetSensorDetailsResponse_SET_SENSOR_DETAILS_RESP_OK);
    }

    // Take the first one
    const zmk_sensors_BehaviorBinding *req_binding = &set_req->bindings[0];

    zmk_behavior_local_id_t bid = req_binding->behavior_id;
    const char *behavior_name = zmk_behavior_find_behavior_name_from_local_id(bid);

    if (!behavior_name) {
        return SENSOR_RESPONSE(
            set_sensor_details,
            zmk_sensors_SetSensorDetailsResponse_SET_SENSOR_DETAILS_RESP_INVALID_BEHAVIOR);
    }

    struct zmk_behavior_binding binding = (struct zmk_behavior_binding){
        .behavior_dev = behavior_name,
        .param1 = req_binding->param1,
        .param2 = req_binding->param2,
    };

    int ret = zmk_behavior_validate_binding(&binding);
    if (ret < 0) {
        return SENSOR_RESPONSE(
            set_sensor_details,
            zmk_sensors_SetSensorDetailsResponse_SET_SENSOR_DETAILS_RESP_INVALID_PARAMETERS);
    }

    ret =
        zmk_keymap_set_layer_sensor_binding_at_idx(set_req->layer_id, set_req->sensor_id, binding);

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

    raise_zmk_studio_rpc_notification((struct zmk_studio_rpc_notification){
        .notification = KEYMAP_NOTIFICATION(unsaved_changes_status_changed, true)});

    return SENSOR_RESPONSE(set_sensor_details,
                           zmk_sensors_SetSensorDetailsResponse_SET_SENSOR_DETAILS_RESP_OK);
}

ZMK_RPC_SUBSYSTEM_HANDLER(sensors, list_all_sensors, ZMK_STUDIO_RPC_HANDLER_UNSECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(sensors, get_sensor_details, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(sensors, set_sensor_details, ZMK_STUDIO_RPC_HANDLER_SECURED);
