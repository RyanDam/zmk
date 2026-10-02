/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zephyr/drivers/usb/usb_buf.h>
#include <zephyr/usb/class/usbd_hid.h>

#include <zmk/usb.h>
#include <zmk/hid.h>
#include <zmk/keymap.h>

#if IS_ENABLED(CONFIG_ZMK_POINTING_SMOOTH_SCROLLING)
#include <zmk/pointing/resolution_multipliers.h>
#endif // IS_ENABLED(CONFIG_ZMK_POINTING_SMOOTH_SCROLLING)

#if IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)
#include <zmk/hid_indicators.h>
#endif // IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)

#include <zmk/event_manager.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/* Must match in-report-size of the zmk_usb_hid devicetree node */
#define ZMK_USB_HID_REPORT_MAX_SIZE 64

static const struct device *const hid_dev = DEVICE_DT_GET(DT_NODELABEL(zmk_usb_hid));

static K_SEM_DEFINE(hid_sem, 1, 1);

/*
 * The new USB device stack requires input reports to be aligned to
 * USB_BUF_ALIGN, while ZMK report structs are packed (alignment 1). Reports
 * are copied into this staging buffer before submission; the semaphore
 * guarantees at most one in-flight report at a time.
 */
static uint8_t hid_report_staging[ZMK_USB_HID_REPORT_MAX_SIZE] __aligned(USB_BUF_ALIGN);

#if IS_ENABLED(CONFIG_ZMK_USB_BOOT)
static uint8_t hid_protocol = HID_PROTOCOL_REPORT;

static void set_proto_cb(const struct device *dev, const uint8_t protocol) {
    hid_protocol = protocol;
}

void zmk_usb_hid_set_protocol(uint8_t protocol) { hid_protocol = protocol; }
#endif /* IS_ENABLED(CONFIG_ZMK_USB_BOOT) */

static uint8_t *get_keyboard_report(size_t *len) {
#if IS_ENABLED(CONFIG_ZMK_USB_BOOT)
    if (hid_protocol != HID_PROTOCOL_REPORT) {
        zmk_hid_boot_report_t *boot_report = zmk_hid_get_boot_report();
        *len = sizeof(*boot_report);
        return (uint8_t *)boot_report;
    }
#endif
    struct zmk_hid_keyboard_report *report = zmk_hid_get_keyboard_report();
    *len = sizeof(*report);
    return (uint8_t *)report;
}

static int get_report_cb(const struct device *dev, const uint8_t type, const uint8_t id,
                         const uint16_t len, uint8_t *const buf) {
    switch (type) {
    case HID_REPORT_TYPE_FEATURE:
        switch (id) {
#if IS_ENABLED(CONFIG_ZMK_POINTING_SMOOTH_SCROLLING)
        case ZMK_HID_REPORT_ID_MOUSE: {
            static struct zmk_hid_mouse_resolution_feature_report res_feature_report;

            struct zmk_endpoint_instance endpoint = {
                .transport = ZMK_TRANSPORT_USB,
            };

            struct zmk_pointing_resolution_multipliers mult =
                zmk_pointing_resolution_multipliers_get_profile(endpoint);

            res_feature_report.body.wheel_res = mult.wheel;
            res_feature_report.body.hwheel_res = mult.hor_wheel;
            memcpy(buf, &res_feature_report, MIN(len, sizeof(res_feature_report)));
            return MIN(len, sizeof(res_feature_report));
        }
#endif // IS_ENABLED(CONFIG_ZMK_POINTING_SMOOTH_SCROLLING)
        default:
            return -ENOTSUP;
        }
        break;

    case HID_REPORT_TYPE_INPUT:
        switch (id) {
        case ZMK_HID_REPORT_ID_KEYBOARD: {
            size_t size;
            uint8_t *report = get_keyboard_report(&size);
            memcpy(buf, report, MIN(len, size));
            return MIN(len, size);
        }
        case ZMK_HID_REPORT_ID_CONSUMER: {
            struct zmk_hid_consumer_report *report = zmk_hid_get_consumer_report();
            memcpy(buf, report, MIN(len, sizeof(*report)));
            return MIN(len, sizeof(*report));
        }
        default:
            LOG_ERR("Invalid report ID %d requested", id);
            return -EINVAL;
        }
        break;

    default:
        /*
         * 7.2.1 of the HID v1.11 spec is unclear about handling requests for reports that do not
         * exist. For requested reports that aren't input reports, return -ENOTSUP like the Zephyr
         * subsys does.
         */
        LOG_ERR("Unsupported report type %d requested", type);
        return -ENOTSUP;
    }
}

static int set_report_cb(const struct device *dev, const uint8_t type, const uint8_t id,
                         const uint16_t len, const uint8_t *const buf) {
    switch (type) {
    case HID_REPORT_TYPE_FEATURE:
        switch (id) {
#if IS_ENABLED(CONFIG_ZMK_POINTING_SMOOTH_SCROLLING)
        case ZMK_HID_REPORT_ID_MOUSE:
            if (len != sizeof(struct zmk_hid_mouse_resolution_feature_report)) {
                return -EINVAL;
            }

            struct zmk_hid_mouse_resolution_feature_report *report =
                (struct zmk_hid_mouse_resolution_feature_report *)buf;
            struct zmk_endpoint_instance endpoint = {
                .transport = ZMK_TRANSPORT_USB,
            };

            zmk_pointing_resolution_multipliers_process_report(&report->body, endpoint);

            return 0;
#endif // IS_ENABLED(CONFIG_ZMK_POINTING_SMOOTH_SCROLLING)
        default:
            return -ENOTSUP;
        }
        break;

    case HID_REPORT_TYPE_OUTPUT:
        switch (id) {
#if IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)
        case ZMK_HID_REPORT_ID_LEDS:
            if (len != sizeof(struct zmk_hid_led_report)) {
                LOG_ERR("LED set report is malformed: length=%d", len);
                return -EINVAL;
            } else {
                struct zmk_hid_led_report *report = (struct zmk_hid_led_report *)buf;
                struct zmk_endpoint_instance endpoint = {
                    .transport = ZMK_TRANSPORT_USB,
                };
                zmk_hid_indicators_process_report(&report->body, endpoint);
            }
            return 0;
#endif // IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)
        default:
            LOG_ERR("Invalid report ID %d requested", id);
            return -EINVAL;
        }
        break;

    default:
        LOG_ERR("Unsupported report type %d requested", type);
        return -ENOTSUP;
    }
}

static void in_done_cb(const struct device *dev, const uint8_t *const report) {
    k_sem_give(&hid_sem);
}

static const struct hid_device_ops ops = {
#if IS_ENABLED(CONFIG_ZMK_USB_BOOT)
    .set_protocol = set_proto_cb,
#endif
    .get_report = get_report_cb,
    .set_report = set_report_cb,
    .input_report_done = in_done_cb,
};

static int zmk_usb_hid_send_report(const uint8_t *report, size_t len) {
    if (len > sizeof(hid_report_staging)) {
        LOG_ERR("Report too large: %zu > %zu", len, sizeof(hid_report_staging));
        return -EMSGSIZE;
    }

    switch (zmk_usb_get_status()) {
    case ZMK_USB_STATUS_SUSPENDED:
        return zmk_usb_wakeup_request();
    case ZMK_USB_STATUS_DISCONNECTED:
    case ZMK_USB_STATUS_ERROR:
    case ZMK_USB_STATUS_POWERED:
        return -ENODEV;
    default:
        break;
    }

    k_sem_take(&hid_sem, K_MSEC(30));
    memcpy(hid_report_staging, report, len);
    int err = hid_device_submit_report(hid_dev, len, hid_report_staging);

    if (err) {
        k_sem_give(&hid_sem);
    }

    return err;
}

int zmk_usb_hid_send_keyboard_report(void) {
    size_t len;
    uint8_t *report = get_keyboard_report(&len);
    return zmk_usb_hid_send_report(report, len);
}

int zmk_usb_hid_send_consumer_report(void) {
#if IS_ENABLED(CONFIG_ZMK_USB_BOOT)
    if (hid_protocol == HID_PROTOCOL_BOOT) {
        return -ENOTSUP;
    }
#endif /* IS_ENABLED(CONFIG_ZMK_USB_BOOT) */

    struct zmk_hid_consumer_report *report = zmk_hid_get_consumer_report();
    return zmk_usb_hid_send_report((uint8_t *)report, sizeof(*report));
}

#if IS_ENABLED(CONFIG_ZMK_POINTING)
int zmk_usb_hid_send_mouse_report() {
#if IS_ENABLED(CONFIG_ZMK_USB_BOOT)
    if (hid_protocol == HID_PROTOCOL_BOOT) {
        return -ENOTSUP;
    }
#endif /* IS_ENABLED(CONFIG_ZMK_USB_BOOT) */

    struct zmk_hid_mouse_report *report = zmk_hid_get_mouse_report();
    return zmk_usb_hid_send_report((uint8_t *)report, sizeof(*report));
}
#endif // IS_ENABLED(CONFIG_ZMK_POINTING)

static int zmk_usb_hid_init(void) {
    hid_device_register(hid_dev, zmk_hid_report_desc, sizeof(zmk_hid_report_desc), &ops);

    /* Apply the configured polling interval (devicetree default is 1ms) */
    if (hid_device_set_in_polling(hid_dev, CONFIG_USB_HID_POLL_INTERVAL_MS * 1000)) {
        LOG_WRN("Failed to set USB HID polling period");
    }

    return 0;
}

SYS_INIT(zmk_usb_hid_init, APPLICATION, CONFIG_ZMK_USB_HID_INIT_PRIORITY);
