/*
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#define DT_DRV_COMPAT nxp_mpr121

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/logging/log.h>
#include <zephyr/kernel.h>
#include <stdlib.h>
#include <math.h>

#include "mpr121.h"
#include <zmk/event_manager.h>
#include <zmk/events/mpr121_gesture_event.h>
#include <zmk/events/mpr121_touch_event.h>

LOG_MODULE_REGISTER(mpr121, CONFIG_SENSOR_LOG_LEVEL);

static const int col_electrode_idx[MPR121_NUM_ELECTRODES] = {-1, -1, -1, 5, 4, 3,
                                                             -1, -1, -1, 2, 1, 0};
static const int row_electrode_idx[MPR121_NUM_ELECTRODES] = {0, 1, 2, -1, -1, -1,
                                                             5, 4, 3, -1, -1, -1};

static int mpr121_i2c_write(const struct device *dev, uint8_t reg, uint8_t val) {
    const struct mpr121_config *cfg = dev->config;
    uint8_t buf[2] = {reg, val};
    return i2c_write_dt(&cfg->i2c, buf, sizeof(buf));
}

static int mpr121_i2c_read(const struct device *dev, uint8_t reg, uint8_t *buf, size_t len) {
    const struct mpr121_config *cfg = dev->config;
    return i2c_write_read_dt(&cfg->i2c, &reg, 1, buf, len);
}

static uint16_t mpr121_read_touch_status(const struct device *dev) {
    uint8_t buf[2];
    if (mpr121_i2c_read(dev, MPR121_TOUCHSTATUS_L, buf, 2) < 0) {
        return 0;
    }
    return ((uint16_t)(buf[1] & 0x1F) << 8) | buf[0];
}

static int mpr121_read_filtered_data(const struct device *dev,
                                     uint16_t out[MPR121_NUM_ELECTRODES]) {
    uint8_t buf[MPR121_NUM_ELECTRODES * 2];
    int ret = mpr121_i2c_read(dev, MPR121_FILTDATA_0, buf, sizeof(buf));
    if (ret < 0) {
        return ret;
    }
    for (int i = 0; i < MPR121_NUM_ELECTRODES; i++) {
        out[i] = (uint16_t)buf[i * 2] | ((uint16_t)(buf[i * 2 + 1] & 0x03) << 8);
    }
    return 0;
}

static int mpr121_read_baselines(const struct device *dev, uint16_t out[MPR121_NUM_ELECTRODES]) {
    uint8_t buf[MPR121_NUM_ELECTRODES];
    int ret = mpr121_i2c_read(dev, MPR121_BASELINE_0, buf, sizeof(buf));
    if (ret < 0) {
        return ret;
    }
    for (int i = 0; i < MPR121_NUM_ELECTRODES; i++) {
        out[i] = (uint16_t)buf[i] << 2;
    }
    return 0;
}

static struct mpr121_grid_pos mpr121_calc_position(uint16_t touch_status, float last_x,
                                                   float last_y) {
    struct mpr121_grid_pos pos = {last_x, last_y};

    if (!touch_status) {
        return pos;
    }

    float sum_x = 0.0f;
    int count_x = 0;
    float sum_y = 0.0f;
    int count_y = 0;

    for (int e = 0; e < MPR121_NUM_ELECTRODES; e++) {
        if (!(touch_status & BIT(e))) {
            continue;
        }

        int ci = col_electrode_idx[e];
        int ri = row_electrode_idx[e];

        if (ci >= 0) {
            sum_x += (float)ci;
            count_x++;
        }
        if (ri >= 0) {
            sum_y += (float)ri;
            count_y++;
        }
    }

    if (count_x > 0) {
        pos.x = (sum_x / (float)count_x) / 5.0f;
    }
    if (count_y > 0) {
        pos.y = (sum_y / (float)count_y) / 5.0f;
    }

    return pos;
}

static struct mpr121_grid_pos mpr121_calc_position_weighted(const struct device *dev,
                                                            uint16_t touch_status, float last_x,
                                                            float last_y) {
    struct mpr121_grid_pos pos = {last_x, last_y};

    if (!touch_status) {
        return pos;
    }

    uint16_t filtered[MPR121_NUM_ELECTRODES];
    uint16_t baseline[MPR121_NUM_ELECTRODES];

    if (mpr121_read_filtered_data(dev, filtered) < 0 || mpr121_read_baselines(dev, baseline) < 0) {
        LOG_WRN("Filtered data read failed, falling back to binary centroid");
        return mpr121_calc_position(touch_status, last_x, last_y);
    }

    float sum_wx = 0.0f;
    float total_wx = 0.0f;
    float sum_wy = 0.0f;
    float total_wy = 0.0f;

    for (int e = 0; e < MPR121_NUM_ELECTRODES; e++) {
        if (!(touch_status & BIT(e))) {
            continue;
        }

        float drop = (float)baseline[e] - (float)filtered[e];
        float weight = (drop > 0.0f) ? drop : 0.0f;

        int ci = col_electrode_idx[e];
        int ri = row_electrode_idx[e];

        if (ci >= 0) {
            sum_wx += weight * (float)ci;
            total_wx += weight;
        }
        if (ri >= 0) {
            sum_wy += weight * (float)ri;
            total_wy += weight;
        }
    }

    if (total_wx > 0.0f) {
        pos.x = (sum_wx / total_wx) / 5.0f;
    }
    if (total_wy > 0.0f) {
        pos.y = (sum_wy / total_wy) / 5.0f;
    }

    return pos;
}

static enum mpr121_gesture_type
mpr121_detect_gesture(const struct mpr121_config *cfg, struct mpr121_grid_pos start,
                      struct mpr121_grid_pos end, uint32_t duration_ms, uint16_t *out_displacement,
                      uint16_t *out_velocity) {
    float dx = end.x - start.x;
    float dy = end.y - start.y;
    float abs_dx = fabsf(dx);
    float abs_dy = fabsf(dy);

    float max_disp = fmaxf(abs_dx, abs_dy);
    *out_displacement = (uint16_t)(max_disp);

    if (duration_ms == 0) {
        duration_ms = 1;
    }

    uint16_t velocity = (uint16_t)((max_disp * 1000.0f) / duration_ms);
    *out_velocity = velocity;

    LOG_INF("Gesture displacement %f velocity %f duration %f", (double)max_disp, (double)velocity,
            (double)duration_ms);

    if (max_disp < cfg->gesture_min_displacement) {
        return MPR121_GESTURE_NONE;
    }
    if (duration_ms > cfg->gesture_max_duration_ms) {
        return MPR121_GESTURE_NONE;
    }
    if (velocity < cfg->gesture_min_velocity) {
        return MPR121_GESTURE_NONE;
    }

    if (abs_dx > abs_dy) {
        if (abs_dy / abs_dx > 0.5f) {
            return MPR121_GESTURE_NONE;
        }
        return dx > 0 ? MPR121_GESTURE_SWIPE_RIGHT : MPR121_GESTURE_SWIPE_LEFT;
    }
    if (abs_dx / abs_dy > 0.5f) {
        return MPR121_GESTURE_NONE;
    }
    return dy > 0 ? MPR121_GESTURE_SWIPE_DOWN : MPR121_GESTURE_SWIPE_UP;
}

static void mpr121_handle_touch_end(const struct device *dev) {
    struct mpr121_data *data = dev->data;
    const struct mpr121_config *cfg = dev->config;

    uint32_t end_time = k_uptime_get();
    uint32_t duration_ms = end_time - data->touch_start_time;

    float total_dx = data->last_pos.x - data->start_pos.x;
    float total_dy = data->last_pos.y - data->start_pos.y;
    float total_disp = fmaxf(fabsf(total_dx), fabsf(total_dy));

    raise_zmk_mpr121_touch_end_event((struct zmk_mpr121_touch_end_event){
        .x = data->last_pos.x,
        .y = data->last_pos.y,
        .duration_ms = duration_ms,
    });

    if (duration_ms <= cfg->tap_max_duration_ms &&
        (int)(total_disp * 100) <= cfg->tap_max_displacement) {
        raise_zmk_mpr121_touch_tap_event((struct zmk_mpr121_touch_tap_event){
            .x = data->start_pos.x,
            .y = data->start_pos.y,
            .duration_ms = duration_ms,
        });
    } else {
        uint16_t displacement, velocity;
        enum mpr121_gesture_type gesture = mpr121_detect_gesture(
            cfg, data->start_pos, data->last_pos, duration_ms, &displacement, &velocity);

        if (gesture != MPR121_GESTURE_NONE) {
            raise_zmk_mpr121_gesture_event((struct zmk_mpr121_gesture_event){
                .gesture_type = gesture,
                .displacement = displacement,
                .velocity = velocity,
                .duration_ms = duration_ms,
            });
        }
    }
}

static void mpr121_poll_handler(struct k_work *work) {
    struct k_work_delayable *dwork = k_work_delayable_from_work(work);
    struct mpr121_data *data = CONTAINER_OF(dwork, struct mpr121_data, poll_work);
    const struct device *dev = data->dev;
    const struct mpr121_config *cfg = dev->config;

    uint16_t touch_status = mpr121_read_touch_status(dev);
    bool currently_touched = (touch_status != 0);

    if (!data->is_touched && currently_touched) {
        uint16_t combined_status = touch_status;

        for (int s = 0; s < 4; s++) {
            k_msleep(2);
            uint16_t ts = mpr121_read_touch_status(dev);
            combined_status |= ts;
        }

        struct mpr121_grid_pos pos =
            mpr121_calc_position_weighted(dev, combined_status, 0.5f, 0.5f);
        pos.x *= cfg->movement_scale;
        pos.y *= cfg->movement_scale;

        ab_filter_reset(&data->ab_filter);
        ab_filter_update(&data->ab_filter, pos.x, pos.y, cfg->ab_filter_alpha,
                         (float)cfg->poll_interval_ms);

        data->start_pos = pos;
        data->last_pos = pos;
        data->touch_start_time = k_uptime_get();
        data->is_touched = true;
        data->total_movement = 0;

        raise_zmk_mpr121_touch_start_event((struct zmk_mpr121_touch_start_event){
            .x = pos.x,
            .y = pos.y,
        });
    } else if (data->is_touched && currently_touched) {
        struct mpr121_grid_pos raw_pos =
            mpr121_calc_position_weighted(dev, touch_status, data->last_pos.x / cfg->movement_scale,
                                          data->last_pos.y / cfg->movement_scale);
        raw_pos.x *= cfg->movement_scale;
        raw_pos.y *= cfg->movement_scale;

        ab_filter_update(&data->ab_filter, raw_pos.x, raw_pos.y, cfg->ab_filter_alpha,
                         (float)cfg->poll_interval_ms);
        struct mpr121_grid_pos pos = {
            .x = data->ab_filter.x,
            .y = data->ab_filter.y,
        };
        // struct mpr121_grid_pos pos = {
        //     .x = raw_pos.x,
        //     .y = raw_pos.y,
        // };

        float dx = pos.x - data->last_pos.x;
        float dy = pos.y - data->last_pos.y;

        if (dx != 0.0f || dy != 0.0f) {
            data->total_movement += fabsf(dx) + fabsf(dy);
            raise_zmk_mpr121_touch_move_event((struct zmk_mpr121_touch_move_event){
                .dx = dx,
                .dy = dy,
                .x = pos.x,
                .y = pos.y,
            });
        }

        data->last_pos = pos;
    } else if (data->is_touched && !currently_touched) {
        mpr121_handle_touch_end(dev);
        data->is_touched = false;
        data->total_movement = 0;
    }

    data->last_touch_status = touch_status;

    if (data->is_touched) {
        k_work_schedule(&data->poll_work, K_MSEC(cfg->poll_interval_ms));
    } else {
        int pin_level = gpio_pin_get_dt(&data->config->interrupt_gpio);
        if (pin_level < 0) {
            LOG_ERR("Failed to read IRQ pin level: %d, attempting re-enable", pin_level);
            int ret = gpio_pin_interrupt_configure_dt(&data->config->interrupt_gpio,
                                                      GPIO_INT_EDGE_TO_ACTIVE);
            if (ret < 0) {
                LOG_ERR("Re-enable also failed: %d, falling back to poll", ret);
                k_work_schedule(&data->poll_work, K_MSEC(cfg->poll_interval_ms));
            }
            return;
        }
        if (pin_level == 1) {
            LOG_DBG("IRQ already active, skipping interrupt, restarting poll");
            k_work_reschedule(&data->poll_work, K_MSEC(cfg->poll_interval_ms));
        } else {
            int ret =
                gpio_pin_interrupt_configure_dt(&data->config->interrupt_gpio, GPIO_INT_EDGE_BOTH);
            if (ret < 0) {
                LOG_ERR("Failed to re-enable IRQ interrupt: %d, falling back to poll", ret);
                k_work_schedule(&data->poll_work, K_MSEC(cfg->poll_interval_ms));
                return;
            }
            LOG_DBG("Re-enabled interrupt");
        }
    }
}

static void mpr121_gpio_callback(const struct device *gpio_dev, struct gpio_callback *cb,
                                 uint32_t pins) {
    struct mpr121_data *data = CONTAINER_OF(cb, struct mpr121_data, gpio_cb);

    gpio_pin_interrupt_configure_dt(&data->config->interrupt_gpio, GPIO_INT_DISABLE);
    LOG_DBG("Interrupt fired, disabled, starting poll");

    k_work_reschedule(&data->poll_work, K_NO_WAIT);
}

static int mpr121_init_hw(const struct device *dev) {
    const struct mpr121_config *cfg = dev->config;
    int ret;

    ret = mpr121_i2c_write(dev, MPR121_SOFTRESET, 0x63);
    if (ret < 0) {
        LOG_ERR("Soft reset failed: %d", ret);
        return ret;
    }
    k_msleep(10);

    ret = mpr121_i2c_write(dev, MPR121_ECR, 0x00);
    if (ret < 0) {
        LOG_ERR("Stop mode failed: %d", ret);
        return ret;
    }

    for (int i = 0; i < MPR121_NUM_ELECTRODES; i++) {
        ret = mpr121_i2c_write(dev, MPR121_TOUCHTH_0 + 2 * i, cfg->touch_threshold);
        if (ret < 0) {
            LOG_ERR("Touch threshold failed elec %d: %d", i, ret);
            return ret;
        }
        ret = mpr121_i2c_write(dev, MPR121_RELEASETH_0 + 2 * i, cfg->release_threshold);
        if (ret < 0) {
            LOG_ERR("Release threshold failed elec %d: %d", i, ret);
            return ret;
        }
    }

    mpr121_i2c_write(dev, MPR121_MHDR, 0x01);
    mpr121_i2c_write(dev, MPR121_NHDR, 0x01);
    mpr121_i2c_write(dev, MPR121_NCLR, 0x0E);
    mpr121_i2c_write(dev, MPR121_FDLR, 0x00);
    mpr121_i2c_write(dev, MPR121_MHDF, 0x01);
    mpr121_i2c_write(dev, MPR121_NHDF, 0x05);
    mpr121_i2c_write(dev, MPR121_NCLF, 0x01);
    mpr121_i2c_write(dev, MPR121_FDLF, 0x00);
    mpr121_i2c_write(dev, MPR121_NHDT, 0x00);
    mpr121_i2c_write(dev, MPR121_NCLT, 0x00);
    mpr121_i2c_write(dev, MPR121_FDLT, 0x00);
    mpr121_i2c_write(dev, MPR121_DEBOUNCE, 0x00);
    mpr121_i2c_write(dev, MPR121_CONFIG1, 0x50);
    mpr121_i2c_write(dev, MPR121_CONFIG2, 0x20);

    ret = mpr121_i2c_write(dev, MPR121_ECR, 0x80 | MPR121_NUM_ELECTRODES);
    if (ret < 0) {
        LOG_ERR("Run mode failed: %d", ret);
        return ret;
    }

    LOG_INF("MPR121 hw init (touch=%d release=%d)", cfg->touch_threshold, cfg->release_threshold);
    return 0;
}

static int mpr121_init(const struct device *dev) {
    struct mpr121_data *data = dev->data;
    const struct mpr121_config *cfg = dev->config;

    data->dev = dev;
    data->config = cfg;
    data->last_touch_status = 0;
    data->is_touched = false;
    data->ab_filter = (struct mpr121_ab_filter){0};

    if (!device_is_ready(cfg->i2c.bus)) {
        LOG_ERR("I2C bus %s not ready", cfg->i2c.bus->name);
        return -ENODEV;
    }

    int ret = mpr121_init_hw(dev);
    if (ret < 0) {
        return ret;
    }

    if (device_is_ready(cfg->interrupt_gpio.port)) {
        ret = gpio_pin_configure_dt(&cfg->interrupt_gpio, GPIO_INPUT);
        if (ret < 0) {
            LOG_ERR("Interrupt GPIO config failed: %d", ret);
            return ret;
        }

        gpio_init_callback(&data->gpio_cb, mpr121_gpio_callback, BIT(cfg->interrupt_gpio.pin));
        ret = gpio_add_callback(cfg->interrupt_gpio.port, &data->gpio_cb);
        if (ret < 0) {
            LOG_ERR("Add GPIO callback failed: %d", ret);
            return ret;
        }

        ret = gpio_pin_interrupt_configure_dt(&cfg->interrupt_gpio, GPIO_INT_EDGE_BOTH);
        if (ret < 0) {
            LOG_ERR("GPIO interrupt config failed: %d", ret);
            return ret;
        }
        LOG_INF("Interrupt on %s pin %d", cfg->interrupt_gpio.port->name, cfg->interrupt_gpio.pin);
    } else {
        LOG_WRN("Interrupt GPIO not ready");
    }

    k_work_init_delayable(&data->poll_work, mpr121_poll_handler);
    LOG_INF("MPR121 driver init ok");
    return 0;
}

#define MPR121_INST(n)                                                                             \
    static struct mpr121_data mpr121_data_##n;                                                     \
    static const struct mpr121_config mpr121_cfg_##n = {                                           \
        .i2c = I2C_DT_SPEC_INST_GET(n),                                                            \
        .interrupt_gpio = GPIO_DT_SPEC_INST_GET_OR(n, interrupt_gpios, {0}),                       \
        .touch_threshold = DT_INST_PROP_OR(n, touch_threshold, 4),                                 \
        .release_threshold = DT_INST_PROP_OR(n, release_threshold, 2),                             \
        .gesture_min_displacement = DT_INST_PROP_OR(n, gesture_min_displacement, 100),             \
        .gesture_min_velocity = DT_INST_PROP_OR(n, gesture_min_velocity, 1000),                    \
        .gesture_max_duration_ms = DT_INST_PROP_OR(n, gesture_max_duration_ms, 300),               \
        .poll_interval_ms = DT_INST_PROP_OR(n, poll_interval_ms, 5),                               \
        .tap_max_duration_ms = DT_INST_PROP_OR(n, tap_max_duration_ms, 200),                       \
        .tap_max_displacement = DT_INST_PROP_OR(n, tap_max_displacement, 20),                      \
        .movement_scale = DT_INST_PROP_OR(n, movement_scale, 200),                                 \
        .ab_filter_alpha = DT_INST_PROP_OR(n, ab_filter_alpha, 80) / 100.0f,                       \
    };                                                                                             \
    DEVICE_DT_INST_DEFINE(n, mpr121_init, NULL, &mpr121_data_##n, &mpr121_cfg_##n, POST_KERNEL,    \
                          CONFIG_SENSOR_INIT_PRIORITY, NULL);

DT_INST_FOREACH_STATUS_OKAY(MPR121_INST)
