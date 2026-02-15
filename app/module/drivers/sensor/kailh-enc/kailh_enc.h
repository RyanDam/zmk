/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/util.h>

struct kailh_enc_config {
    const struct gpio_dt_spec a;
    const struct gpio_dt_spec b;

    const uint16_t steps;
    const uint8_t resolution;
};

struct kailh_enc_data {
    uint8_t ab_state;
    int8_t pulses;
    int8_t ticks;
    int8_t delta;

#ifdef CONFIG_KAILH_ENC_TRIGGER
    struct gpio_callback a_gpio_cb;
    struct gpio_callback b_gpio_cb;
    const struct device *dev;

    sensor_trigger_handler_t handler;
    const struct sensor_trigger *trigger;

#if defined(CONFIG_KAILH_ENC_TRIGGER_OWN_THREAD)
    K_THREAD_STACK_MEMBER(thread_stack, CONFIG_KAILH_ENC_THREAD_STACK_SIZE);
    struct k_sem gpio_sem;
    struct k_thread thread;
#elif defined(CONFIG_KAILH_ENC_TRIGGER_GLOBAL_THREAD)
    struct k_work work;
#endif

#endif /* CONFIG_KAILH_ENC_TRIGGER */
};

#ifdef CONFIG_KAILH_ENC_TRIGGER

int kailh_enc_trigger_set(const struct device *dev, const struct sensor_trigger *trig,
                     sensor_trigger_handler_t handler);

int kailh_enc_init_interrupt(const struct device *dev);
#endif