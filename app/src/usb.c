/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>

#include <zephyr/usb/usbd.h>
#include <zephyr/usb/usbd_msg.h>

#include <zmk/hid.h>
#include <zmk/keymap.h>
#include <zmk/event_manager.h>
#include <zmk/events/usb_conn_state_changed.h>

#include <zmk/usb.h>
#include <zmk/usb_hid.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/*
 * ZMK boards alias the USB device controller (UDC) node as zephyr_udc0.
 * The usbd_context is bound to that node.
 */
#if DT_NODE_EXISTS(DT_N_NODELABEL_zephyr_udc0)
#define ZMK_USB_UDC_DEV DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0))
#else
#error                                                                                             \
    "ZMK USB requires the USB controller node to be aliased as zephyr_udc0 in the board devicetree"
#endif

USBD_DEVICE_DEFINE(zmk_usbd, ZMK_USB_UDC_DEV, CONFIG_USB_DEVICE_VID, CONFIG_USB_DEVICE_PID);

USBD_DESC_LANG_DEFINE(zmk_usbd_lang);
USBD_DESC_MANUFACTURER_DEFINE(zmk_usbd_mfr, CONFIG_USB_DEVICE_MANUFACTURER);
USBD_DESC_PRODUCT_DEFINE(zmk_usbd_product, CONFIG_USB_DEVICE_PRODUCT);
USBD_DESC_SERIAL_NUMBER_DEFINE(zmk_usbd_sn);

USBD_DESC_CONFIG_DEFINE(zmk_usbd_fs_cfg_str, "FS Configuration");
USBD_CONFIGURATION_DEFINE(zmk_usbd_fs_config,
                          COND_CODE_1(IS_ENABLED(CONFIG_ZMK_USB_REMOTE_WAKEUP),
                                      (USB_SCD_REMOTE_WAKEUP), (0)),
                          100, &zmk_usbd_fs_cfg_str);

USBD_DESC_CONFIG_DEFINE(zmk_usbd_hs_cfg_str, "HS Configuration");
USBD_CONFIGURATION_DEFINE(zmk_usbd_hs_config,
                          COND_CODE_1(IS_ENABLED(CONFIG_ZMK_USB_REMOTE_WAKEUP),
                                      (USB_SCD_REMOTE_WAKEUP), (0)),
                          100, &zmk_usbd_hs_cfg_str);

/*
 * Bus state, as tracked from usbd messages. `usb_configured` is latched like
 * the legacy stack's is_configured: it stays set from SET_CONFIGURATION until
 * a bus reset or VBUS removal.
 */
static bool usb_connected;
static bool usb_configured;
static bool usb_suspended;

static void raise_usb_status_changed_event(struct k_work *_work) {
    raise_zmk_usb_conn_state_changed(
        (struct zmk_usb_conn_state_changed){.conn_state = zmk_usb_get_conn_state()});
}

K_WORK_DEFINE(usb_status_notifier_work, raise_usb_status_changed_event);

enum zmk_usb_status zmk_usb_get_status(void) {
    if (usb_suspended) {
        return ZMK_USB_STATUS_SUSPENDED;
    }
    if (!usb_connected) {
        return ZMK_USB_STATUS_DISCONNECTED;
    }
    if (usb_configured) {
        return ZMK_USB_STATUS_CONFIGURED;
    }
    return ZMK_USB_STATUS_POWERED;
}

enum zmk_usb_conn_state zmk_usb_get_conn_state(void) {
    // LOG_DBG("connected: %d configured: %d suspended: %d", usb_connected, usb_configured,
    //         usb_suspended);
    if (usb_configured || usb_suspended) {
        return ZMK_USB_CONN_HID;
    }
    if (usb_connected) {
        return ZMK_USB_CONN_POWERED;
    }
    return ZMK_USB_CONN_NONE;
}

bool zmk_usb_is_hid_ready(void) {
    return usb_configured && zmk_usb_get_conn_state() == ZMK_USB_CONN_HID;
}

int zmk_usb_wakeup_request(void) { return usbd_wakeup_request(&zmk_usbd); }

static void usb_msg_cb(struct usbd_context *const ctx, const struct usbd_msg *const msg) {
    bool state_changed = false;

    switch (msg->type) {
    case USBD_MSG_VBUS_READY:
        usb_connected = true;
        state_changed = true;
        break;

    case USBD_MSG_VBUS_REMOVED:
        usb_connected = false;
        usb_configured = false;
        usb_suspended = false;
        state_changed = true;
        break;

    case USBD_MSG_RESET:
        usb_connected = true;
        usb_configured = false;
        usb_suspended = false;
#if IS_ENABLED(CONFIG_ZMK_USB_BOOT)
        zmk_usb_hid_set_protocol(HID_PROTOCOL_REPORT);
#endif
        state_changed = true;
        break;

    case USBD_MSG_CONFIGURATION:
        usb_configured = (msg->status != 0);
        usb_suspended = false;
        state_changed = true;
        break;

    case USBD_MSG_SUSPEND:
        usb_suspended = true;
        state_changed = true;
        break;

    case USBD_MSG_RESUME:
        usb_suspended = false;
        state_changed = true;
        break;

    case USBD_MSG_UDC_ERROR:
    case USBD_MSG_STACK_ERROR:
        usb_configured = false;
        usb_suspended = false;
        state_changed = true;
        break;

    default:
        break;
    }

    if (state_changed) {
        k_work_submit(&usb_status_notifier_work);
    }
}

static int zmk_usb_register_classes(struct usbd_context *const ctx, const enum usbd_speed speed,
                                    struct usbd_config_node *const cfg) {
    int err;

    err = usbd_add_configuration(ctx, speed, cfg);
    if (err) {
        LOG_ERR("Failed to add USB configuration (%d)", err);
        return err;
    }

#if IS_ENABLED(CONFIG_ZMK_USB)
    err = usbd_register_class(ctx, "hid_0", speed, 1);
    if (err) {
        LOG_ERR("Failed to register USB HID class (%d)", err);
        return err;
    }
#endif

    /* Register all CDC ACM instances (USB logging, Studio RPC over USB-CDC) */
    for (int i = 0; i < DT_NUM_INST_STATUS_OKAY(zephyr_cdc_acm_uart); i++) {
        char name[16];

        snprintf(name, sizeof(name), "cdc_acm_%d", i);
        err = usbd_register_class(ctx, name, speed, 1);
        if (err) {
            LOG_ERR("Failed to register USB CDC ACM class %d (%d)", i, err);
            return err;
        }
    }

    return 0;
}

static int zmk_usb_init(void) {
    int usb_enable_ret;

    int err = usbd_add_descriptor(&zmk_usbd, &zmk_usbd_lang);
    if (err) {
        LOG_ERR("Failed to add USB language descriptor (%d)", err);
        return -EINVAL;
    }

    err = usbd_add_descriptor(&zmk_usbd, &zmk_usbd_mfr);
    if (err) {
        LOG_ERR("Failed to add USB manufacturer descriptor (%d)", err);
        return -EINVAL;
    }

    err = usbd_add_descriptor(&zmk_usbd, &zmk_usbd_product);
    if (err) {
        LOG_ERR("Failed to add USB product descriptor (%d)", err);
        return -EINVAL;
    }

    err = usbd_add_descriptor(&zmk_usbd, &zmk_usbd_sn);
    if (err) {
        LOG_ERR("Failed to add USB serial number descriptor (%d)", err);
        return -EINVAL;
    }

    if (USBD_SUPPORTS_HIGH_SPEED && usbd_caps_speed(&zmk_usbd) == USBD_SPEED_HS) {
        err = zmk_usb_register_classes(&zmk_usbd, USBD_SPEED_HS, &zmk_usbd_hs_config);
        if (err) {
            return err;
        }
    }

    err = zmk_usb_register_classes(&zmk_usbd, USBD_SPEED_FS, &zmk_usbd_fs_config);
    if (err) {
        return err;
    }

    err = usbd_msg_register_cb(&zmk_usbd, usb_msg_cb);
    if (err) {
        LOG_ERR("Failed to register USB message callback (%d)", err);
        return -EINVAL;
    }

    err = usbd_init(&zmk_usbd);
    if (err) {
        LOG_ERR("Failed to initialize USB device support (%d)", err);
        return -EINVAL;
    }

    usb_enable_ret = usbd_enable(&zmk_usbd);

    if (usb_enable_ret != 0) {
        LOG_ERR("Unable to enable USB");
        return -EINVAL;
    }

    return 0;
}

SYS_INIT(zmk_usb_init, APPLICATION, CONFIG_ZMK_USB_INIT_PRIORITY);
