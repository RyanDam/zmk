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

#include "kailh_enc.h"

extern struct kailh_enc_data kailh_enc_driver;

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(KAILH_ENC, CONFIG_SENSOR_LOG_LEVEL);

static inline void setup_int(const struct device *dev, bool enable) {
    const struct kailh_enc_config *cfg = dev->config;

    // LOG_DBG("Interrupt enabled %s", (enable ? "true" : "false"));

    if (gpio_pin_interrupt_configure_dt(&cfg->a, enable ? GPIO_INT_EDGE_BOTH : GPIO_INT_DISABLE)) {
        LOG_WRN("Unable to set A pin GPIO interrupt");
    }

    if (gpio_pin_interrupt_configure_dt(&cfg->b, enable ? GPIO_INT_EDGE_BOTH : GPIO_INT_DISABLE)) {
        LOG_WRN("Unable to set A pin GPIO interrupt");
    }
}

static void kailh_enc_a_gpio_callback(const struct device *dev, struct gpio_callback *cb,
                                      uint32_t pins) {
    struct kailh_enc_data *drv_data = CONTAINER_OF(cb, struct kailh_enc_data, a_gpio_cb);

    // LOG_DBG("");

    setup_int(drv_data->dev, false);

    k_timer_stop(&drv_data->debounce_timer);
    k_timer_start(&drv_data->debounce_timer, K_MSEC(drv_data->debounce_period_ms), K_NO_WAIT);
}

static void kailh_enc_b_gpio_callback(const struct device *dev, struct gpio_callback *cb,
                                      uint32_t pins) {
    struct kailh_enc_data *drv_data = CONTAINER_OF(cb, struct kailh_enc_data, b_gpio_cb);

    // LOG_DBG("");

    setup_int(drv_data->dev, false);

    k_timer_stop(&drv_data->debounce_timer);
    k_timer_start(&drv_data->debounce_timer, K_MSEC(drv_data->debounce_period_ms), K_NO_WAIT);
}

static void kailh_enc_debounce_timer_handler(struct k_timer *timer) {
    struct kailh_enc_data *drv_data = CONTAINER_OF(timer, struct kailh_enc_data, debounce_timer);

    setup_int(drv_data->dev, false);

#if defined(CONFIG_KAILH_ENC_TRIGGER_OWN_THREAD)
    k_sem_give(&drv_data->gpio_sem);
#elif defined(CONFIG_KAILH_ENC_TRIGGER_GLOBAL_THREAD)
    k_work_submit(&drv_data->work);
#endif
}

static void kailh_enc_thread_cb(const struct device *dev) {
    struct kailh_enc_data *drv_data = dev->data;

    drv_data->handler(dev, drv_data->trigger);

    setup_int(dev, true);
}

#ifdef CONFIG_KAILH_ENC_TRIGGER_OWN_THREAD
static void kailh_enc_thread(int dev_ptr, int unused) {
    const struct device *dev = INT_TO_POINTER(dev_ptr);
    struct kailh_enc_data *drv_data = dev->data;

    ARG_UNUSED(unused);

    while (1) {
        k_sem_take(&drv_data->gpio_sem, K_FOREVER);
        kailh_enc_thread_cb(dev);
    }
}
#endif

#ifdef CONFIG_KAILH_ENC_TRIGGER_GLOBAL_THREAD
static void kailh_enc_work_cb(struct k_work *work) {
    struct kailh_enc_data *drv_data = CONTAINER_OF(work, struct kailh_enc_data, work);

    // LOG_DBG("");

    kailh_enc_thread_cb(drv_data->dev);
}
#endif

int kailh_enc_trigger_set(const struct device *dev, const struct sensor_trigger *trig,
                          sensor_trigger_handler_t handler) {
    struct kailh_enc_data *drv_data = dev->data;

    setup_int(dev, false);

    k_msleep(5);

    drv_data->trigger = trig;
    drv_data->handler = handler;
    drv_data->debounce_period_ms = CONFIG_KAILH_ENC_DEBOUNCE_PERIOD;

    setup_int(dev, true);

    return 0;
}

int kailh_enc_init_interrupt(const struct device *dev) {
    struct kailh_enc_data *drv_data = dev->data;
    const struct kailh_enc_config *drv_cfg = dev->config;

    drv_data->dev = dev;
    k_timer_init(&drv_data->debounce_timer, kailh_enc_debounce_timer_handler, NULL);

    /* setup gpio interrupt */

    gpio_init_callback(&drv_data->a_gpio_cb, kailh_enc_a_gpio_callback, BIT(drv_cfg->a.pin));

    if (gpio_add_callback(drv_cfg->a.port, &drv_data->a_gpio_cb) < 0) {
        LOG_DBG("Failed to set A callback!");
        return -EIO;
    }

    gpio_init_callback(&drv_data->b_gpio_cb, kailh_enc_b_gpio_callback, BIT(drv_cfg->b.pin));

    if (gpio_add_callback(drv_cfg->b.port, &drv_data->b_gpio_cb) < 0) {
        LOG_DBG("Failed to set B callback!");
        return -EIO;
    }

#if defined(CONFIG_KAILH_ENC_TRIGGER_OWN_THREAD)
    k_sem_init(&drv_data->gpio_sem, 0, UINT_MAX);

    k_thread_create(&drv_data->thread, drv_data->thread_stack, CONFIG_KAILH_ENC_THREAD_STACK_SIZE,
                    (k_thread_entry_t)kailh_enc_thread, dev, 0, NULL,
                    K_PRIO_COOP(CONFIG_KAILH_ENC_THREAD_PRIORITY), 0, K_NO_WAIT);
#elif defined(CONFIG_KAILH_ENC_TRIGGER_GLOBAL_THREAD)
    k_work_init(&drv_data->work, kailh_enc_work_cb);
#endif

    return 0;
}