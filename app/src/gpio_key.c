/*
 * Copyright (c) 2025 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <zephyr/logging/log.h>

#include <zmk/ble.h>
#include <indicator/indicator.h>
#include <zmk/debounce.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define DT_DRV_COMPAT zmk_gpio_key

#define GPIO_KEY_SCAN_MS 30
#define GPIO_KEY_TAP_THRESHOLD_MS 500
#define GPIO_KEY_HOLD_THRESHOLD_MS 7000


struct gpio_key_config {
    struct gpio_dt_spec gpio;
    bool pull_up;
    bool pull_down;
    uint32_t debounce_ms;
};

struct gpio_key_data {
    struct gpio_callback callback;
    struct k_work_delayable work;
    struct zmk_debounce_state state;
    const struct device *dev;
    uint32_t press_start_time;
    bool is_hold_processed;
    bool is_pressing;
};

static const struct zmk_debounce_config debounce_config = {
    .debounce_press_ms = 10,
    .debounce_release_ms = 10,
};

static void gpio_key_work_handler(struct k_work *work) {
    struct k_work_delayable *dwork = k_work_delayable_from_work(work);
    struct gpio_key_data *data = CONTAINER_OF(dwork, struct gpio_key_data, work);
    const struct gpio_key_config *config = data->dev->config;
    uint32_t current_time = k_uptime_get();
    uint32_t elapsed_ms = current_time - data->state.counter;

    int current_state = gpio_pin_get_dt(&config->gpio);
    if (current_state < 0) {
        LOG_ERR("Failed to read GPIO pin state (%d)", current_state);
        k_work_reschedule(&data->work, K_MSEC(GPIO_KEY_SCAN_MS));
        return;
    }

    bool active = (current_state == 0);
    zmk_debounce_update(&data->state, active, elapsed_ms, &debounce_config);

    if (zmk_debounce_get_changed(&data->state)) {
        if (zmk_debounce_is_pressed(&data->state)) {
            data->is_pressing = true;
            data->is_hold_processed = false;
            data->press_start_time = current_time;
            LOG_DBG("GPIO key pressed on pin %d", config->gpio.pin);
        } else {
            uint32_t press_duration = current_time - data->press_start_time;

            if (press_duration < GPIO_KEY_TAP_THRESHOLD_MS && !data->is_hold_processed) {
                LOG_DBG("GPIO key tap detected on pin %d", config->gpio.pin);
                zmk_ble_prof_next();
            }

            data->is_pressing = false;
            LOG_DBG("GPIO key released on pin %d", config->gpio.pin);
        }
    }

    if (data->is_pressing && !data->is_hold_processed) {
        uint32_t press_duration = current_time - data->press_start_time;
        if (press_duration >= GPIO_KEY_HOLD_THRESHOLD_MS) {
            LOG_DBG("GPIO key hold reached threshold (%d ms) on pin %d", GPIO_KEY_HOLD_THRESHOLD_MS,
                    config->gpio.pin);
            zmk_ble_clear_bonds();
            indicate_connectivity();
            data->is_hold_processed = true;
            data->is_pressing = false;
        }
    }

    if (zmk_debounce_is_active(&data->state)) {
        k_work_reschedule(&data->work, K_MSEC(GPIO_KEY_SCAN_MS));
    } else {
        gpio_pin_interrupt_configure_dt(&config->gpio, GPIO_INT_EDGE_TO_INACTIVE);
    }
}

static void gpio_key_irq_handler(const struct device *port, struct gpio_callback *cb,
                                 const gpio_port_pins_t pin) {
    ARG_UNUSED(port);
    ARG_UNUSED(pin);

    struct gpio_key_data *data =
        (struct gpio_key_data *)CONTAINER_OF(cb, struct gpio_key_data, callback);

    const struct gpio_key_config *config = (const struct gpio_key_config *)data->dev->config;
    gpio_pin_interrupt_configure_dt(&config->gpio, GPIO_INT_DISABLE);
    k_work_reschedule(&data->work, K_NO_WAIT);
}

static int gpio_key_init(const struct device *dev) {
    const struct gpio_key_config *config = dev->config;
    struct gpio_key_data *data = dev->data;

    LOG_DBG("gpio: %s %d, pull up: %d, pull down: %d, debounce-ms: %dms", config->gpio.port->name,
            config->gpio.pin, config->pull_up, config->pull_down, config->debounce_ms);

    gpio_flags_t flags = GPIO_INPUT;
    if (config->pull_up) {
        flags |= GPIO_PULL_UP;
    } else if (config->pull_down) {
        flags |= GPIO_PULL_DOWN;
    }

    int ret = gpio_pin_configure_dt(&config->gpio, flags);
    if (ret < 0) {
        LOG_ERR("Failed to configure GPIO key pin (%d)", ret);
        return ret;
    }

    data->dev = dev;
    k_work_init_delayable(&data->work, gpio_key_work_handler);
    gpio_init_callback(&data->callback, gpio_key_irq_handler, BIT(config->gpio.pin));

    ret = gpio_add_callback(config->gpio.port, &data->callback);
    if (ret < 0) {
        LOG_ERR("Failed to add GPIO callback (%d)", ret);
        return ret;
    }

    data->state.pressed = false;
    data->state.changed = false;
    data->state.counter = 0;
    data->is_pressing = false;
    data->is_hold_processed = false;

    gpio_pin_interrupt_configure_dt(&config->gpio, GPIO_INT_EDGE_TO_INACTIVE);

    LOG_INF("GPIO key driver initialized on pin %d", config->gpio.pin);
    return 0;
}

#define GPIO_KEY_INST(n)                                                                           \
    const struct gpio_key_config gpio_key_cfg_##n = {                                              \
        .gpio = GPIO_DT_SPEC_INST_GET(n, gpios),                                                   \
        .pull_up = DT_INST_PROP_OR(n, pull_up, false),                                             \
        .pull_down = DT_INST_PROP_OR(n, pull_down, false),                                         \
        .debounce_ms = DT_INST_PROP_OR(n, debounce_ms, 0),                                         \
    };                                                                                             \
    static struct gpio_key_data gpio_key_data_##n;                                                 \
    DEVICE_DT_INST_DEFINE(n, gpio_key_init, NULL, &gpio_key_data_##n, &gpio_key_cfg_##n,           \
                          PRE_KERNEL_2, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, NULL);

DT_INST_FOREACH_STATUS_OKAY(GPIO_KEY_INST)