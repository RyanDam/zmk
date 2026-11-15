/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * AXS15231B integrated capacitive touch controller driver.
 *
 * The AXS15231B LCD chip integrates a capacitive touch controller that is
 * reachable over I2C at address 0x3B. On the Guition JC3248W535 reference
 * board and the cobantab1a module it is wired to I2C0 with no interrupt or
 * reset line, so the driver polls it periodically.
 *
 * Read protocol (ported from the reference bsp):
 *   1. Write 11 bytes (no control phase):
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

/* No INT line is wired, so poll at ~100 Hz. */
#define AXS_TOUCH_POLL_PERIOD_MS 10

/* I2C read command: request 8 bytes (one point record). */
static const uint8_t axs_touch_read_cmd[] = {
    0xB5, 0xAB, 0xA5, 0x5A, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00,
};

#define AXS_TOUCH_DATA_LEN 8

struct axs15231b_touch_config {
    struct input_touchscreen_common_config common;
    struct i2c_dt_spec bus;
};

struct axs15231b_touch_data {
    const struct device *dev;
    struct k_work work;
    struct k_timer timer;
    bool touched;
    uint16_t last_x;
    uint16_t last_y;
};

INPUT_TOUCH_STRUCT_CHECK(struct axs15231b_touch_config);

static void axs15231b_touch_process(const struct device *dev) {
    struct axs15231b_touch_data *data = dev->data;
    const struct axs15231b_touch_config *cfg = dev->config;
    uint8_t resp[AXS_TOUCH_DATA_LEN];
    int err;

    /*
     * The reference bsp issues the command write and the data read as two
     * separate I2C transactions (a STOP between them), not a single
     * write-then-read with a repeated start. Match that: the integrated
     * touch controller does not answer a repeated-start read.
     */
    err = i2c_write_dt(&cfg->bus, axs_touch_read_cmd, sizeof(axs_touch_read_cmd));
    if (err) {
        LOG_ERR("Touch write failed: %d", err);
        return;
    }

    err = i2c_read_dt(&cfg->bus, resp, sizeof(resp));
    if (err) {
        LOG_ERR("Touch read failed: %d", err);
        return;
    }

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

    if (!i2c_is_ready_dt(&cfg->bus)) {
        LOG_ERR("I2C bus not ready for AXS15231B touch");
        return -ENODEV;
    }

    data->dev = dev;
    k_work_init(&data->work, axs15231b_touch_work_handler);
    k_timer_init(&data->timer, axs15231b_touch_timer_handler, NULL);
    k_timer_start(&data->timer, K_MSEC(AXS_TOUCH_POLL_PERIOD_MS), K_MSEC(AXS_TOUCH_POLL_PERIOD_MS));

    LOG_INF("AXS15231B touch initialized (polling every %d ms)", AXS_TOUCH_POLL_PERIOD_MS);
    return 0;
}

#define AXS15231B_TOUCH_INIT(index)                                                                \
    static const struct axs15231b_touch_config axs15231b_touch_config_##index = {                  \
        .common = INPUT_TOUCH_DT_INST_COMMON_CONFIG_INIT(index),                                   \
        .bus = I2C_DT_SPEC_INST_GET(index),                                                        \
    };                                                                                             \
    static struct axs15231b_touch_data axs15231b_touch_data_##index;                               \
    DEVICE_DT_INST_DEFINE(index, axs15231b_touch_init, NULL, &axs15231b_touch_data_##index,        \
                          &axs15231b_touch_config_##index, POST_KERNEL,                            \
                          CONFIG_INPUT_INIT_PRIORITY, NULL);

DT_INST_FOREACH_STATUS_OKAY(AXS15231B_TOUCH_INIT)
