/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_kscan_mock

#include "kscan_input.h"

#include <stdlib.h>
#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <dt-bindings/zmk/kscan_mock.h>

/* Delay before exiting after the last event. The input thread (higher
 * priority than the system work queue) consumes the reported events and the
 * downstream consumers process them before this delay elapses. */
#define KSCAN_MOCK_EXIT_DELAY_MS 10

struct kscan_mock_data {
    uint32_t event_index;
    struct k_work_delayable work;
    struct k_work_delayable exit_work;
    const struct device *dev;
};

static void kscan_mock_exit_handler(struct k_work *work) { exit(0); }

#define MOCK_INST_INIT(n)                                                                          \
    struct kscan_mock_config_##n {                                                                 \
        uint32_t events[DT_INST_PROP_LEN(n, events)];                                              \
        bool exit_after;                                                                           \
    };                                                                                             \
    static void kscan_mock_schedule_next_event_##n(const struct device *dev) {                     \
        struct kscan_mock_data *data = dev->data;                                                  \
        const struct kscan_mock_config_##n *cfg = dev->config;                                     \
        if (data->event_index < DT_INST_PROP_LEN(n, events)) {                                     \
            uint32_t ev = cfg->events[data->event_index];                                          \
            LOG_DBG("delaying next keypress: %d", ZMK_MOCK_MSEC(ev));                              \
            k_work_schedule(&data->work, K_MSEC(ZMK_MOCK_MSEC(ev)));                               \
        } else if (cfg->exit_after) {                                                              \
            LOG_DBG("Exiting");                                                                    \
            k_work_schedule(&data->exit_work, K_MSEC(KSCAN_MOCK_EXIT_DELAY_MS));                   \
        }                                                                                          \
    }                                                                                              \
    static void kscan_mock_work_handler_##n(struct k_work *work) {                                 \
        struct k_work_delayable *d_work = k_work_delayable_from_work(work);                        \
        struct kscan_mock_data *data = CONTAINER_OF(d_work, struct kscan_mock_data, work);         \
        const struct kscan_mock_config_##n *cfg = data->dev->config;                               \
        if (data->event_index >= DT_INST_PROP_LEN(n, events)) {                                    \
            if (cfg->exit_after) {                                                                 \
                k_work_schedule(&data->exit_work, K_MSEC(KSCAN_MOCK_EXIT_DELAY_MS));               \
            }                                                                                      \
            return;                                                                                \
        }                                                                                          \
        uint32_t ev = cfg->events[data->event_index];                                              \
        LOG_DBG("ev %u row %d column %d state %d\n", ev, ZMK_MOCK_ROW(ev), ZMK_MOCK_COL(ev),       \
                ZMK_MOCK_IS_PRESS(ev));                                                            \
        zmk_kscan_input_report(data->dev, ZMK_MOCK_ROW(ev), ZMK_MOCK_COL(ev),                      \
                               ZMK_MOCK_IS_PRESS(ev));                                             \
        kscan_mock_schedule_next_event_##n(data->dev);                                             \
        data->event_index++;                                                                       \
    }                                                                                              \
    static int kscan_mock_init_##n(const struct device *dev) {                                     \
        struct kscan_mock_data *data = dev->data;                                                  \
        data->dev = dev;                                                                           \
        k_work_init_delayable(&data->work, kscan_mock_work_handler_##n);                           \
        k_work_init_delayable(&data->exit_work, kscan_mock_exit_handler);                          \
        /* No PM: start emitting autonomously */                                                   \
        kscan_mock_schedule_next_event_##n(dev);                                                   \
        return 0;                                                                                  \
    }                                                                                              \
    static struct kscan_mock_data kscan_mock_data_##n;                                             \
    static const struct kscan_mock_config_##n kscan_mock_config_##n = {                            \
        .events = DT_INST_PROP(n, events), .exit_after = DT_INST_PROP(n, exit_after)};             \
    DEVICE_DT_INST_DEFINE(n, kscan_mock_init_##n, NULL, &kscan_mock_data_##n,                      \
                          &kscan_mock_config_##n, POST_KERNEL, CONFIG_ZMK_KSCAN_INIT_PRIORITY,     \
                          NULL);

DT_INST_FOREACH_STATUS_OKAY(MOCK_INST_INIT)
