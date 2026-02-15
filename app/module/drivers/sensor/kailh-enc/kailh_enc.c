/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT kailh_enc

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/util.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/sys/__assert.h>
#include <zephyr/logging/log.h>

#include "kailh_enc.h"

#define FULL_ROTATION 360

LOG_MODULE_REGISTER(KAILH_ENC, CONFIG_SENSOR_LOG_LEVEL);

static int kailh_enc_get_ab_state(const struct device *dev) {
    const struct kailh_enc_config *drv_cfg = dev->config;

    return (gpio_pin_get_dt(&drv_cfg->a) << 1) | gpio_pin_get_dt(&drv_cfg->b);
}

static int kailh_enc_sample_fetch(const struct device *dev, enum sensor_channel chan) {
    struct kailh_enc_data *drv_data = dev->data;
    const struct kailh_enc_config *drv_cfg = dev->config;
    uint8_t val;
    int8_t delta;

    __ASSERT_NO_MSG(chan == SENSOR_CHAN_ALL || chan == SENSOR_CHAN_ROTATION);

    val = kailh_enc_get_ab_state(dev);

    LOG_DBG("prev: %d, new: %d", drv_data->ab_state, val);

    switch (val | (drv_data->ab_state << 2)) {
    case 0b0010: // 0 -> 2, 2
    case 0b0100: // 1 -> 0, 4
    case 0b1101: // 3 -> 1, 13
    case 0b1011: // 2 -> 3, 11
    case 0b1100: // 3 -> 0, 12
    case 0b1111: // 3 -> 3, 15
    case 0b0101: // 1 -> 1, 5
        delta = -1;
        break;
    case 0b0001: // 0 -> 1, 1
    case 0b0111: // 1 -> 3, 7
    case 0b1110: // 3 -> 2, 14
    case 0b1000: // 2 -> 0, 8
    case 0b1010: // 2 -> 2, 10 
    case 0b1001: // 2 -> 1, 9
        delta = 1;
        break;
    default:
        delta = 0;
        break;
    }

    LOG_DBG("Delta: %d", delta);

    drv_data->pulses += delta;
    drv_data->ab_state = val;

    // TODO: Temporary code for backwards compatibility to support
    // the sensor channel rotation reporting *ticks* instead of delta of degrees.
    // REMOVE ME
    if (drv_cfg->steps == 0) {
        drv_data->ticks = drv_data->pulses / drv_cfg->resolution;
        drv_data->delta = delta;
        drv_data->pulses %= drv_cfg->resolution;
    }

    return 0;
}

static int kailh_enc_channel_get(const struct device *dev, enum sensor_channel chan,
                            struct sensor_value *val) {
    struct kailh_enc_data *drv_data = dev->data;
    const struct kailh_enc_config *drv_cfg = dev->config;
    int32_t pulses = drv_data->pulses;

    if (chan != SENSOR_CHAN_ROTATION) {
        return -ENOTSUP;
    }

    drv_data->pulses = 0;

    if (drv_cfg->steps > 0) {
        val->val1 = (pulses * FULL_ROTATION) / drv_cfg->steps;
        val->val2 = (pulses * FULL_ROTATION) % drv_cfg->steps;
        if (val->val2 != 0) {
            val->val2 *= 1000000;
            val->val2 /= drv_cfg->steps;
        }
    } else {
        val->val1 = drv_data->ticks;
        val->val2 = drv_data->delta;
    }

    return 0;
}

static const struct sensor_driver_api kailh_enc_driver_api = {
#ifdef CONFIG_KAILH_ENC_TRIGGER
    .trigger_set = kailh_enc_trigger_set,
#endif
    .sample_fetch = kailh_enc_sample_fetch,
    .channel_get = kailh_enc_channel_get,
};

int kailh_enc_init(const struct device *dev) {
    struct kailh_enc_data *drv_data = dev->data;
    const struct kailh_enc_config *drv_cfg = dev->config;

    LOG_DBG("A: %s %d B: %s %d resolution %d", drv_cfg->a.port->name, drv_cfg->a.pin,
            drv_cfg->b.port->name, drv_cfg->b.pin, drv_cfg->resolution);

    if (!device_is_ready(drv_cfg->a.port)) {
        LOG_ERR("A GPIO device is not ready");
        return -EINVAL;
    }

    if (!device_is_ready(drv_cfg->b.port)) {
        LOG_ERR("B GPIO device is not ready");
        return -EINVAL;
    }

    if (gpio_pin_configure_dt(&drv_cfg->a, GPIO_INPUT)) {
        LOG_DBG("Failed to configure A pin");
        return -EIO;
    }

    if (gpio_pin_configure_dt(&drv_cfg->b, GPIO_INPUT)) {
        LOG_DBG("Failed to configure B pin");
        return -EIO;
    }

#ifdef CONFIG_KAILH_ENC_TRIGGER
    if (kailh_enc_init_interrupt(dev) < 0) {
        LOG_DBG("Failed to initialize interrupt!");
        return -EIO;
    }
#endif

    drv_data->ab_state = kailh_enc_get_ab_state(dev);

    return 0;
}

#define KAILH_ENC_INST(n)                                                                           \
    static struct kailh_enc_data kailh_enc_data_##n;                                                 \
    static const struct kailh_enc_config kailh_enc_cfg_##n = {                                       \
        .a = GPIO_DT_SPEC_INST_GET(n, a_gpios),                                                    \
        .b = GPIO_DT_SPEC_INST_GET(n, b_gpios),                                                    \
        .resolution = DT_INST_PROP_OR(n, resolution, 1),                                           \
        .steps = DT_INST_PROP_OR(n, steps, 0),                                                     \
    };                                                                                             \
    DEVICE_DT_INST_DEFINE(n, kailh_enc_init, NULL, &kailh_enc_data_##n, &kailh_enc_cfg_##n, POST_KERNEL, \
                          CONFIG_SENSOR_INIT_PRIORITY, &kailh_enc_driver_api);

DT_INST_FOREACH_STATUS_OKAY(KAILH_ENC_INST)