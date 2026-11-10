/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <errno.h>
#include <string.h>

#include <pb_encode.h>

#include <zmk/activity.h>
#include <zmk/endpoints.h>
#include <zmk/endpoints_types.h>
#include <zmk/event_manager.h>
#include <zmk/studio/rpc.h>

#if IS_ENABLED(CONFIG_ZMK_BLE)
#include <zmk/ble.h>
#include <zmk/events/ble_active_profile_changed.h>
#endif

#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)
#include <zmk/battery.h>
#include <zmk/events/battery_state_changed.h>
#endif

#if IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)
#include <dt-bindings/zmk/hid_indicators.h>
#include <zmk/events/hid_indicators_changed.h>
#include <zmk/hid_indicators.h>
#endif

#if IS_ENABLED(CONFIG_ZMK_SPLIT) && IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
#include <zmk/split/central.h>
#endif

#if IS_ENABLED(CONFIG_ZMK_USB_DEVICE)
#include <zmk/usb.h>
#endif

#if IS_ENABLED(CONFIG_ZMK_WPM)
#include <zmk/wpm.h>
#endif

#include <zmk/events/activity_state_changed.h>
#include <zmk/events/endpoint_changed.h>

LOG_MODULE_DECLARE(zmk_studio, CONFIG_ZMK_STUDIO_LOG_LEVEL);

ZMK_RPC_SUBSYSTEM(settings)

#define SETTINGS_RESPONSE(type, ...) ZMK_RPC_RESPONSE(settings, type, __VA_ARGS__)
#define SETTINGS_NOTIFICATION(type, ...) ZMK_RPC_NOTIFICATION(settings, type, __VA_ARGS__)

static zmk_settings_ActivityState activity_state_to_proto(enum zmk_activity_state state) {
    switch (state) {
    case ZMK_ACTIVITY_ACTIVE:
        return zmk_settings_ActivityState_ACTIVITY_STATE_ACTIVE;
    case ZMK_ACTIVITY_IDLE:
        return zmk_settings_ActivityState_ACTIVITY_STATE_IDLE;
    case ZMK_ACTIVITY_SLEEP:
        return zmk_settings_ActivityState_ACTIVITY_STATE_SLEEP;
    }

    return zmk_settings_ActivityState_ACTIVITY_STATE_ACTIVE;
}

static zmk_settings_Transport transport_to_proto(enum zmk_transport transport) {
    switch (transport) {
    case ZMK_TRANSPORT_USB:
        return zmk_settings_Transport_TRANSPORT_USB;
    case ZMK_TRANSPORT_BLE:
        return zmk_settings_Transport_TRANSPORT_BLE;
    case ZMK_TRANSPORT_NONE:
    default:
        return zmk_settings_Transport_TRANSPORT_NONE;
    }
}

static enum zmk_transport proto_to_transport(zmk_settings_Transport transport) {
    switch (transport) {
    case zmk_settings_Transport_TRANSPORT_USB:
        return ZMK_TRANSPORT_USB;
    case zmk_settings_Transport_TRANSPORT_BLE:
        return ZMK_TRANSPORT_BLE;
    case zmk_settings_Transport_TRANSPORT_NONE:
    default:
        return ZMK_TRANSPORT_NONE;
    }
}

#if IS_ENABLED(CONFIG_ZMK_SPLIT) && IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
static zmk_settings_SplitConnectionsStatus
split_connections_to_proto(enum zmk_split_transport_connections_status status) {
    switch (status) {
    case ZMK_SPLIT_TRANSPORT_CONNECTIONS_STATUS_ALL_CONNECTED:
        return zmk_settings_SplitConnectionsStatus_SPLIT_CONNECTIONS_STATUS_ALL_CONNECTED;
    case ZMK_SPLIT_TRANSPORT_CONNECTIONS_STATUS_SOME_CONNECTED:
        return zmk_settings_SplitConnectionsStatus_SPLIT_CONNECTIONS_STATUS_SOME_CONNECTED;
    case ZMK_SPLIT_TRANSPORT_CONNECTIONS_STATUS_DISCONNECTED:
    default:
        return zmk_settings_SplitConnectionsStatus_SPLIT_CONNECTIONS_STATUS_DISCONNECTED;
    }
}
#endif

static bool usb_powered(void) {
#if IS_ENABLED(CONFIG_ZMK_USB_DEVICE)
    return zmk_usb_is_powered();
#else
    return false;
#endif
}

static void fill_bluetooth_settings(zmk_settings_BluetoothSettings *bluetooth) {
#if IS_ENABLED(CONFIG_ZMK_BLE)
    bluetooth->active_profile = (uint32_t)zmk_ble_active_profile_index();

    uint8_t count = MIN(ZMK_BLE_PROFILE_COUNT, (int)ARRAY_SIZE(bluetooth->profiles));
    bluetooth->profiles_count = count;

    for (uint8_t i = 0; i < count; i++) {
        zmk_settings_BluetoothProfile *profile = &bluetooth->profiles[i];
        bt_addr_le_t *addr = zmk_ble_profile_address(i);

        profile->index = i;
        profile->bonded = !zmk_ble_profile_is_open(i);
        profile->connected = zmk_ble_profile_is_connected(i);
        profile->active = (i == zmk_ble_active_profile_index());
        profile->address_type = (addr && addr->type == BT_ADDR_LE_RANDOM)
                                    ? zmk_settings_AddressType_ADDRESS_TYPE_RANDOM
                                    : zmk_settings_AddressType_ADDRESS_TYPE_PUBLIC;

        if (addr && bt_addr_le_cmp(addr, BT_ADDR_LE_ANY) != 0) {
            profile->peer_address.size = sizeof(addr->a.val);
            memcpy(profile->peer_address.bytes, addr->a.val, sizeof(addr->a.val));
        }

        char *name = zmk_ble_profile_name(i);
        if (name && name[0] != '\0') {
            strncpy(profile->label, name, sizeof(profile->label) - 1);
        }
    }
#else
    bluetooth->active_profile = 0;
    bluetooth->profiles_count = 0;
#endif
}

static void fill_sleep_settings(zmk_settings_SleepSettings *sleep) {
    struct zmk_activity_config config = zmk_activity_get_config();

    sleep->supported = IS_ENABLED(CONFIG_ZMK_SLEEP);
    sleep->enabled = config.sleep_enabled && IS_ENABLED(CONFIG_ZMK_SLEEP);
    sleep->idle_timeout_ms = config.idle_timeout_ms;
    sleep->sleep_timeout_ms = IS_ENABLED(CONFIG_ZMK_SLEEP) ? config.sleep_timeout_ms : 0;
    sleep->activity_state = activity_state_to_proto(zmk_activity_get_state());
    sleep->usb_powered = usb_powered();
}

static void fill_endpoint_settings(zmk_settings_EndpointSettings *endpoint) {
    struct zmk_endpoint_instance selected = zmk_endpoint_get_selected();

    endpoint->preferred_transport = transport_to_proto(zmk_endpoint_get_preferred_transport());
    endpoint->selected_transport = transport_to_proto(selected.transport);
    endpoint->connected = zmk_endpoint_is_connected();
    endpoint->selected_ble_profile =
        selected.transport == ZMK_TRANSPORT_BLE ? (uint32_t)selected.ble.profile_index : 0;
}

#if IS_ENABLED(CONFIG_ZMK_SPLIT) && IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
static void fill_split_peripherals(zmk_settings_SplitDiagnostics *split) {
    uint8_t sources[ZMK_SPLIT_CENTRAL_PERIPHERAL_COUNT];
    int count = zmk_split_central_get_available_source_ids(sources, ARRAY_SIZE(sources));
    if (count < 0) {
        count = 0;
    }

    count = MIN(count, (int)ARRAY_SIZE(split->peripherals));
    split->peripherals_count = count;

    for (int i = 0; i < count; i++) {
        zmk_settings_SplitPeripheral *peripheral = &split->peripherals[i];
        peripheral->source = sources[i];
        peripheral->connected = true;

#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)
        uint8_t level = 0;
        if (zmk_split_central_get_peripheral_battery_level(sources[i], &level) == 0 && level > 0) {
            peripheral->battery_known = true;
            peripheral->battery_level = level;
        }
#endif
    }
}
#endif

static void fill_diagnostics(zmk_settings_Diagnostics *diagnostics) {
#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)
    diagnostics->battery_supported = true;
    diagnostics->battery_level = zmk_battery_state_of_charge();
#endif

    diagnostics->usb_powered = usb_powered();

#if IS_ENABLED(CONFIG_ZMK_WPM)
    diagnostics->wpm_supported = true;
    diagnostics->wpm = (uint32_t)zmk_wpm_get_state();
#endif

#if IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)
    diagnostics->hid_indicators_supported = true;
    diagnostics->has_hid_indicators = true;

    zmk_hid_indicators_t indicators = zmk_hid_indicators_get_current_profile();
    diagnostics->hid_indicators.raw = indicators;
    diagnostics->hid_indicators.num_lock = (indicators & HID_INDICATOR_NUM_LOCK) != 0;
    diagnostics->hid_indicators.caps_lock = (indicators & HID_INDICATOR_CAPS_LOCK) != 0;
    diagnostics->hid_indicators.scroll_lock = (indicators & HID_INDICATOR_SCROLL_LOCK) != 0;
    diagnostics->hid_indicators.compose = (indicators & HID_INDICATOR_COMPOSE) != 0;
    diagnostics->hid_indicators.kana = (indicators & HID_INDICATOR_KANA) != 0;
#endif

#if IS_ENABLED(CONFIG_ZMK_SPLIT) && IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    diagnostics->split_supported = true;
    diagnostics->has_split = true;

    struct zmk_split_transport_status status;
    if (zmk_split_central_get_status(&status) == 0) {
        diagnostics->split.available = status.available;
        diagnostics->split.enabled = status.enabled;
        diagnostics->split.connections = split_connections_to_proto(status.connections);
    } else {
        diagnostics->split.available = false;
        diagnostics->split.enabled = false;
        diagnostics->split.connections =
            zmk_settings_SplitConnectionsStatus_SPLIT_CONNECTIONS_STATUS_DISCONNECTED;
    }

    fill_split_peripherals(&diagnostics->split);
#endif

    diagnostics->uptime_ms = (uint32_t)k_uptime_get();
    diagnostics->activity_state = activity_state_to_proto(zmk_activity_get_state());
}

static void fill_settings(zmk_settings_Settings *settings) {
    settings->has_bluetooth = true;
    fill_bluetooth_settings(&settings->bluetooth);

    settings->has_sleep = true;
    fill_sleep_settings(&settings->sleep);

    settings->has_endpoint = true;
    fill_endpoint_settings(&settings->endpoint);

    settings->has_diagnostics = true;
    fill_diagnostics(&settings->diagnostics);
}

zmk_studio_Response get_settings(const zmk_studio_Request *req) {
    zmk_settings_Settings settings = zmk_settings_Settings_init_zero;
    fill_settings(&settings);

    return SETTINGS_RESPONSE(get_settings, settings);
}

static zmk_settings_SettingsResult
apply_bluetooth_settings(const zmk_settings_SetBluetoothSettingsRequest *request) {
#if IS_ENABLED(CONFIG_ZMK_BLE)
    bool has_action = request->clear_all_bonds || request->has_active_profile ||
                      request->clear_bond_profiles_count > 0 ||
                      request->disconnect_profiles_count > 0;

    if (!has_action) {
        return zmk_settings_SettingsResult_SETTINGS_RESULT_OK;
    }

    if (request->clear_all_bonds) {
        zmk_ble_clear_all_bonds();
    }

    for (pb_size_t i = 0; i < request->clear_bond_profiles_count; i++) {
        uint32_t index = request->clear_bond_profiles[i];
        if (index >= ZMK_BLE_PROFILE_COUNT) {
            return zmk_settings_SettingsResult_SETTINGS_RESULT_INVALID_VALUE;
        }

        int ret = zmk_ble_profile_clear_bond(index);
        if (ret < 0) {
            return zmk_settings_SettingsResult_SETTINGS_RESULT_BLE_ERROR;
        }
    }

    for (pb_size_t i = 0; i < request->disconnect_profiles_count; i++) {
        uint32_t index = request->disconnect_profiles[i];
        if (index >= ZMK_BLE_PROFILE_COUNT) {
            return zmk_settings_SettingsResult_SETTINGS_RESULT_INVALID_VALUE;
        }

        int ret = zmk_ble_prof_disconnect(index);
        if (ret < 0 && ret != -ENODEV) {
            return zmk_settings_SettingsResult_SETTINGS_RESULT_BLE_ERROR;
        }
    }

    if (request->has_active_profile) {
        if (request->active_profile.value >= ZMK_BLE_PROFILE_COUNT) {
            return zmk_settings_SettingsResult_SETTINGS_RESULT_INVALID_VALUE;
        }

        int ret = zmk_ble_prof_select(request->active_profile.value);
        if (ret < 0) {
            return zmk_settings_SettingsResult_SETTINGS_RESULT_BLE_ERROR;
        }
    }

    return zmk_settings_SettingsResult_SETTINGS_RESULT_OK;
#else
    if (request->clear_all_bonds || request->has_active_profile ||
        request->clear_bond_profiles_count > 0 || request->disconnect_profiles_count > 0) {
        return zmk_settings_SettingsResult_SETTINGS_RESULT_UNSUPPORTED;
    }

    return zmk_settings_SettingsResult_SETTINGS_RESULT_OK;
#endif
}

static zmk_settings_SettingsResult
apply_sleep_settings(const zmk_settings_SetSleepSettingsRequest *request) {
    struct zmk_activity_config config = zmk_activity_get_config();

    if (request->has_enabled) {
        if (request->enabled.value && !IS_ENABLED(CONFIG_ZMK_SLEEP)) {
            return zmk_settings_SettingsResult_SETTINGS_RESULT_UNSUPPORTED;
        }

        config.sleep_enabled = request->enabled.value;
    }

    if (request->has_idle_timeout_ms) {
        if (request->idle_timeout_ms.value == 0) {
            return zmk_settings_SettingsResult_SETTINGS_RESULT_INVALID_VALUE;
        }

        config.idle_timeout_ms = request->idle_timeout_ms.value;
    }

    if (request->has_sleep_timeout_ms) {
        if (!IS_ENABLED(CONFIG_ZMK_SLEEP)) {
            return zmk_settings_SettingsResult_SETTINGS_RESULT_UNSUPPORTED;
        }

        if (request->sleep_timeout_ms.value == 0) {
            return zmk_settings_SettingsResult_SETTINGS_RESULT_INVALID_VALUE;
        }

        config.sleep_timeout_ms = request->sleep_timeout_ms.value;
    }

    int ret = zmk_activity_set_config(&config);
    if (ret == -ENOTSUP) {
        return zmk_settings_SettingsResult_SETTINGS_RESULT_UNSUPPORTED;
    }

    if (ret < 0) {
        return zmk_settings_SettingsResult_SETTINGS_RESULT_INVALID_VALUE;
    }

    return zmk_settings_SettingsResult_SETTINGS_RESULT_OK;
}

static zmk_settings_SettingsResult
apply_endpoint_settings(const zmk_settings_SetEndpointSettingsRequest *request) {
    if (!request->has_preferred_transport) {
        return zmk_settings_SettingsResult_SETTINGS_RESULT_OK;
    }

    enum zmk_transport transport = proto_to_transport(request->preferred_transport.value);

    switch (transport) {
    case ZMK_TRANSPORT_USB:
        if (!IS_ENABLED(CONFIG_ZMK_USB)) {
            return zmk_settings_SettingsResult_SETTINGS_RESULT_UNSUPPORTED;
        }
        break;
    case ZMK_TRANSPORT_BLE:
        if (!IS_ENABLED(CONFIG_ZMK_BLE)) {
            return zmk_settings_SettingsResult_SETTINGS_RESULT_UNSUPPORTED;
        }
        break;
    case ZMK_TRANSPORT_NONE:
    default:
        return zmk_settings_SettingsResult_SETTINGS_RESULT_INVALID_VALUE;
    }

    int ret = zmk_endpoint_set_preferred_transport(transport);
    if (ret < 0) {
        return zmk_settings_SettingsResult_SETTINGS_RESULT_INVALID_VALUE;
    }

    return zmk_settings_SettingsResult_SETTINGS_RESULT_OK;
}

zmk_studio_Response set_settings(const zmk_studio_Request *req) {
    const zmk_settings_SetSettingsRequest *request =
        &req->subsystem.settings.request_type.set_settings;

    zmk_settings_SetSettingsResponse response = zmk_settings_SetSettingsResponse_init_zero;
    response.result = zmk_settings_SettingsResult_SETTINGS_RESULT_OK;

    if (request->has_bluetooth) {
        response.result = apply_bluetooth_settings(&request->bluetooth);
        if (response.result != zmk_settings_SettingsResult_SETTINGS_RESULT_OK) {
            goto respond;
        }
    }

    if (request->has_sleep) {
        response.result = apply_sleep_settings(&request->sleep);
        if (response.result != zmk_settings_SettingsResult_SETTINGS_RESULT_OK) {
            goto respond;
        }
    }

    if (request->has_endpoint) {
        response.result = apply_endpoint_settings(&request->endpoint);
        if (response.result != zmk_settings_SettingsResult_SETTINGS_RESULT_OK) {
            goto respond;
        }
    }

respond:
    response.has_settings = true;
    fill_settings(&response.settings);

    return SETTINGS_RESPONSE(set_settings, response);
}

static int settings_settings_reset(void) { return zmk_activity_reset_config(); }

ZMK_RPC_SUBSYSTEM_SETTINGS_RESET(settings, settings_settings_reset);

ZMK_RPC_SUBSYSTEM_HANDLER(settings, get_settings, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(settings, set_settings, ZMK_STUDIO_RPC_HANDLER_SECURED);

/*
 * Settings change notifications are sent asynchronously and coalesced: any
 * trigger only reschedules a single delayed work item, and the notification
 * is raised (and the response sent) from the work queue after the triggering
 * event has fully unwound. This matters because set_settings can raise these
 * events itself (e.g. a BLE profile switch), and sending a response
 * re-entrantly from within request handling overflows the RPC thread stack.
 *
 * The notification is intentionally just a boolean signal: the client
 * re-requests the full settings snapshot on receipt, which avoids copying and
 * encoding a large settings struct through the event path.
 */
#define SETTINGS_NOTIFY_DEBOUNCE_MS 100

static void settings_notify_work_handler(struct k_work *work) {
    zmk_studio_Notification notification = SETTINGS_NOTIFICATION(settings_changed, true);
    raise_zmk_studio_rpc_notification(
        (struct zmk_studio_rpc_notification){.notification = notification});
}

K_WORK_DELAYABLE_DEFINE(settings_notify_work, settings_notify_work_handler);

static int settings_event_listener(const zmk_event_t *eh) {
    k_work_reschedule(&settings_notify_work, K_MSEC(SETTINGS_NOTIFY_DEBOUNCE_MS));
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(settings_notify, settings_event_listener);
ZMK_SUBSCRIPTION(settings_notify, zmk_activity_state_changed);
#if IS_ENABLED(CONFIG_ZMK_BLE)
ZMK_SUBSCRIPTION(settings_notify, zmk_ble_active_profile_changed);
#endif
#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)
ZMK_SUBSCRIPTION(settings_notify, zmk_battery_state_changed);
#endif
#if IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)
ZMK_SUBSCRIPTION(settings_notify, zmk_hid_indicators_changed);
#endif
ZMK_SUBSCRIPTION(settings_notify, zmk_endpoint_changed);
