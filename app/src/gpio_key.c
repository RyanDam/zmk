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

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define DT_DRV_COMPAT zmk_gpio_key

struct gpio_key_config {
    struct gpio_dt_spec gpio;
    bool pull_up;
    bool pull_down;
    uint32_t debounce_ms;
};

static int gpio_key_init(const struct device *dev) {
    const struct gpio_key_config *config = dev->config;

    LOG_DBG("gpio: %s %d, pull up: %d, pull down: %d, debounce-ms: %dms", config->gpio.port->name, config->gpio.pin, config->pull_up, config->pull_down, config->debounce_ms);

    // Configure GPIO as input
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

    LOG_INF("GPIO key driver initialized on pin %d", config->gpio.pin);
    return 0;
}

static void gpio_key_monitor_thread(void *d0, void *d1, void *d2) {
    ARG_UNUSED(d0);
    ARG_UNUSED(d1);
    ARG_UNUSED(d2);

    const struct device *dev = d0;
    const struct gpio_key_config *config = dev->config;
    int previous_state = -1; // Initialize to invalid state
    uint32_t last_debounce_time = 0;
    bool is_pressing = false; // Track if key is currently pressed
    bool is_hold_processed = false; // Track if hold action has been processed
    uint32_t press_start_time = 0; // Time when press started
    uint32_t tap_threshold_ms = 500; // 500ms tap threshold
    uint32_t hold_threshold_ms = 7000; // 7000ms tap threshold

    LOG_INF("Starting GPIO key monitoring thread on pin %d", config->gpio.pin);

    while (true) {
        // Read current GPIO state
        int current_state = gpio_pin_get_dt(&config->gpio);

        // Handle error case
        if (current_state < 0) {
            LOG_ERR("Failed to read GPIO pin state (%d)", current_state);
            k_sleep(K_MSEC(100));
            continue;
        }

        // Handle debounce
        if (config->debounce_ms > 0 && current_state != previous_state) {
            uint32_t current_time = k_uptime_get();
            if ((current_time - last_debounce_time) < (config->debounce_ms)) {
                // Still in debounce period, skip logging
                k_sleep(K_MSEC(10));
                continue;
            }
            last_debounce_time = current_time;
        }

        // Check for state change
        if (current_state != previous_state) {
            if (previous_state == -1) {
                // First read, initialize previous state
                previous_state = current_state;
                LOG_DBG("GPIO key state initialized to %d", current_state);
            } else {
                // State changed, process the event
                const char *state_str = current_state ? "HIGH" : "LOW";
                LOG_INF("GPIO key state changed to %s on pin %d", state_str, config->gpio.pin);
                
                if (current_state == 0) {
                    // GPIO went LOW (pressed)
                    is_pressing = true;
                    is_hold_processed = false; // Reset hold processed flag
                    press_start_time = k_uptime_get();
                } else if (current_state == 1 && is_pressing) {
                    // GPIO went HIGH (released after being pressed)
                    uint32_t press_duration = k_uptime_get() - press_start_time;
                    
                    if (press_duration < tap_threshold_ms && !is_hold_processed) {
                        // Tap detected (pressed and released quickly, not a hold)
                        LOG_INF("GPIO key tap detected on pin %d", config->gpio.pin);
                        zmk_ble_prof_next();
                    }
                    
                    is_pressing = false;
                }
                
                previous_state = current_state;
            }
        }

        // Check for hold condition (GPIO still LOW for more than threshold)
        if (is_pressing && current_state == 0) {
            uint32_t current_time = k_uptime_get();
            uint32_t press_duration = current_time - press_start_time;
            
            if (press_duration >= hold_threshold_ms && !is_hold_processed) {
                // Hold detected
                LOG_INF("GPIO key hold reached threshold (%d ms) on pin %d", 
                        hold_threshold_ms, config->gpio.pin);
                zmk_ble_clear_bonds();
                is_pressing = false;
                is_hold_processed = true; // Mark hold as processed
            }
        }

        // Sleep for a short interval to avoid busy-waiting
        k_sleep(K_MSEC(10));
    }
}

#define GPIO_KEY_INST(n)                                                                          \
    const struct gpio_key_config gpio_key_cfg_##n = {                                            \
        .gpio = GPIO_DT_SPEC_INST_GET(n, gpios),                                                 \
        .pull_up = DT_INST_PROP_OR(n, pull_up, false),                                          \
        .pull_down = DT_INST_PROP_OR(n, pull_down, false),                                      \
        .debounce_ms = DT_INST_PROP_OR(n, debounce_ms, 0),                                      \
    };                                                                                            \
    DEVICE_DT_INST_DEFINE(n, gpio_key_init, NULL, NULL, &gpio_key_cfg_##n,                       \
                          PRE_KERNEL_2, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, NULL);             \
    K_THREAD_DEFINE(gpio_key_thread_##n, 1024, gpio_key_monitor_thread,                         \
                    DEVICE_DT_INST_GET(n), NULL, NULL,                                           \
                    K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);

DT_INST_FOREACH_STATUS_OKAY(GPIO_KEY_INST)