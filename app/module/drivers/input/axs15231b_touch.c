/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * AXS15231B integrated capacitive touch controller driver.
 *
 * The AXS15231B LCD chip integrates a capacitive touch controller with its
 * own host interface: the datasheet's "I2C/SPI in AXS15231 is the interface
 * for touch communication with HOST" is a separate slave port on the chip
 * (chip pins GPIO0=SCL, GPIO1=SDA in I2C work mode), not the display's
 * QSPI bus. On the cobantab1a module those lines reach the ESP32-S3 at
 * SDA GPIO4 / SCL GPIO8, the wiring the Guition JC3248W535 reference bsp
 * uses (its EXAMPLE_PIN_NUM_QSPI_TOUCH_SDA/SCL). There is no INT or RST
 * line wired, so the driver polls.
 *
 * Bring-up note: on the cobantab1a unit this interface never answers - a
 * full-bus scan gets zero ACKs on both pad pairings, and the vendor demo
 * shows no touch response on the same module either. The carrier rev in
 * hand does not wire the pads, so the node is disabled; the driver waits
 * for the rev that does. See the devicetree node's comment and
 * esp-port/axs15231b-bringup-findings.md section 18.
 *
 * Read protocol (ported from the reference bsp):
 *   1. Write 11 bytes:
 *        {0xB5, 0xAB, 0xA5, 0x5A, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00}
 *   2. Repeated start, read 8 bytes:
 *        [0] gesture
 *        [1] number of touch points
 *        [2] x high nibble (bits 3:0)
 *        [3] x low byte
 *        [4] y high nibble (bits 3:0)
 *        [5] y low byte
 */

#define DT_DRV_COMPAT axs_axs15231b_touch

#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/input/input.h>
#include <zephyr/input/input_touch.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(axs15231b_touch, CONFIG_INPUT_LOG_LEVEL);

/* No INT line is wired, so poll. */
#define AXS_TOUCH_POLL_PERIOD_MS 20

#define AXS_TOUCH_REQUEST_LEN 11
#define AXS_TOUCH_DATA_LEN 8

static const uint8_t axs_touch_request[AXS_TOUCH_REQUEST_LEN] = {
    0xB5, 0xAB, 0xA5, 0x5A, 0x00, 0x00,
    (uint8_t)(AXS_TOUCH_DATA_LEN >> 8), (uint8_t)(AXS_TOUCH_DATA_LEN & 0xFF),
    0x00, 0x00, 0x00,
};

struct axs15231b_touch_config {
    struct input_touchscreen_common_config common;
    const struct device *bus;
    uint8_t address;
};

struct axs15231b_touch_data {
    const struct device *dev;
    struct k_work work;
    struct k_timer timer;
    bool touched;
    uint16_t last_x;
    uint16_t last_y;
    bool last_ok;
    unsigned int errors_logged;
};

INPUT_TOUCH_STRUCT_CHECK(struct axs15231b_touch_config);

static int axs15231b_touch_read(const struct device *dev, uint8_t *resp) {
    const struct axs15231b_touch_config *cfg = dev->config;

    return i2c_write_read(cfg->bus, cfg->address, axs_touch_request,
                          sizeof(axs_touch_request), resp, AXS_TOUCH_DATA_LEN);
}

static void axs15231b_touch_process(const struct device *dev) {
    struct axs15231b_touch_data *data = dev->data;
    uint8_t resp[AXS_TOUCH_DATA_LEN];
    int err;

    err = axs15231b_touch_read(dev, resp);
    if (err) {
        if (err != -EIO || !data->last_ok) {
            data->last_ok = false;
            if (data->errors_logged < 3) {
                data->errors_logged++;
                LOG_ERR("Touch read failed: %d%s", err,
                        data->errors_logged == 3 ? " (further errors silent)" : "");
            }
        }
        return;
    }
    if (!data->last_ok) {
        LOG_INF("touch controller answering");
    }
    data->last_ok = true;
    data->errors_logged = 0;

    if (resp[1] > 0) {
        uint16_t x = ((resp[2] & 0x0F) << 8) | resp[3];
        uint16_t y = ((resp[4] & 0x0F) << 8) | resp[5];

        data->touched = true;
        data->last_x = x;
        data->last_y = y;

        input_touchscreen_report_pos(dev, x, y, K_NO_WAIT);
        input_report_key(dev, INPUT_BTN_TOUCH, 1, true, K_NO_WAIT);
    } else if (data->touched) {
        data->touched = false;

        /* Report the last position with the touch button released. */
        input_touchscreen_report_pos(dev, data->last_x, data->last_y, K_NO_WAIT);
        input_report_key(dev, INPUT_BTN_TOUCH, 0, true, K_NO_WAIT);
    }
}

static void axs15231b_touch_work_handler(struct k_work *work) {
    struct axs15231b_touch_data *data = CONTAINER_OF(work, struct axs15231b_touch_data, work);

    axs15231b_touch_process(data->dev);
}

static void axs15231b_touch_timer_handler(struct k_timer *timer) {
    struct axs15231b_touch_data *data = CONTAINER_OF(timer, struct axs15231b_touch_data, timer);

    k_work_submit(&data->work);
}

static int axs15231b_touch_init(const struct device *dev) {
    struct axs15231b_touch_data *data = dev->data;
    const struct axs15231b_touch_config *cfg = dev->config;

    if (!device_is_ready(cfg->bus)) {
        LOG_ERR("I2C bus not ready");
        return -ENODEV;
    }

    data->dev = dev;
    k_work_init(&data->work, axs15231b_touch_work_handler);
    k_timer_init(&data->timer, axs15231b_touch_timer_handler, NULL);
    k_timer_start(&data->timer, K_MSEC(AXS_TOUCH_POLL_PERIOD_MS),
                  K_MSEC(AXS_TOUCH_POLL_PERIOD_MS));

    LOG_INF("AXS15231B touch initialized (polling 0x%02X on %s every %d ms)", cfg->address,
            cfg->bus->name, AXS_TOUCH_POLL_PERIOD_MS);
    return 0;
}

#define AXS15231B_TOUCH_INIT(index)                                                                \
    static const struct axs15231b_touch_config axs15231b_touch_config_##index = {                  \
        .common = INPUT_TOUCH_DT_INST_COMMON_CONFIG_INIT(index),                                   \
        .bus = DEVICE_DT_GET(DT_BUS(DT_INST(index, axs_axs15231b_touch))),                         \
        .address = DT_REG_ADDR(DT_INST(index, axs_axs15231b_touch)),                               \
    };                                                                                             \
    static struct axs15231b_touch_data axs15231b_touch_data_##index;                               \
    DEVICE_DT_INST_DEFINE(index, axs15231b_touch_init, NULL, &axs15231b_touch_data_##index,        \
                          &axs15231b_touch_config_##index, POST_KERNEL,                            \
                          CONFIG_INPUT_INIT_PRIORITY, NULL);

DT_INST_FOREACH_STATUS_OKAY(AXS15231B_TOUCH_INIT)
