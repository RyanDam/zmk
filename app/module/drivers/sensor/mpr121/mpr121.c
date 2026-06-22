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

static const int col_electrodes[MPR121_NUM_COLS] = {11, 10, 9, 5, 4, 3};
static const int row_electrodes[MPR121_NUM_ROWS] = {0, 1, 2, 8, 7, 6};

static int electrode_to_col(int electrode) {
    for (int i = 0; i < MPR121_NUM_COLS; i++) {
        if (col_electrodes[i] == electrode) {
            return i;
        }
    }
    return -1;
}

static int electrode_to_row(int electrode) {
    for (int i = 0; i < MPR121_NUM_ROWS; i++) {
        if (row_electrodes[i] == electrode) {
            return i;
        }
    }
    return -1;
}

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
    if (mpr121_i2c_read(dev, MPR121_TOUCHSTATUS_L, &buf[0], 1) < 0) {
        return 0;
    }
    if (mpr121_i2c_read(dev, MPR121_TOUCHSTATUS_H, &buf[1], 1) < 0) {
        return 0;
    }
    return (buf[1] << 8) | (buf[0] & 0x0FFF);
}

static float mpr121_kf_update(float *est, float *cov, float measurement, float Q, float R) {
    *cov += Q;
    float K = *cov / (*cov + R);
    *est = *est + K * (measurement - *est);
    *cov = (1.0f - K) * (*cov);
    return *est;
}

static struct mpr121_grid_pos mpr121_calc_position(uint16_t touch_status, float last_x,
                                                   float last_y) {
    struct mpr121_grid_pos pos = {last_x, last_y};

    float sum_x = 0.0f;
    float sum_y = 0.0f;
    int count_x = 0;
    int count_y = 0;

    for (int e = 0; e < MPR121_NUM_ELECTRODES; e++) {
        if (!(touch_status & BIT(e))) {
            continue;
        }

        int col = electrode_to_col(e);
        int row = electrode_to_row(e);

        if (col >= 0) {
            sum_x += col;
            count_x++;
        }
        if (row >= 0) {
            sum_y += row;
            count_y++;
        }
    }

    if (count_x > 0) {
        pos.x = (sum_x / count_x) / 5.0f;
    }
    if (count_y > 0) {
        pos.y = (sum_y / count_y) / 5.0f;
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
        return dx > 0 ? MPR121_GESTURE_SWIPE_RIGHT : MPR121_GESTURE_SWIPE_LEFT;
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

    // LOG_INF("Touch end: dur=%u disp=%d", duration_ms, (int)(total_disp * 100));

    raise_zmk_mpr121_touch_end_event((struct zmk_mpr121_touch_end_event){
        .x = data->last_pos.x,
        .y = data->last_pos.y,
        .duration_ms = duration_ms,
    });

    if (duration_ms <= cfg->tap_max_duration_ms &&
        (int)(total_disp * 100) <= cfg->tap_max_displacement) {
        // LOG_INF("Tap: x=%d y=%d", (int)(data->start_pos.x * 100), (int)(data->start_pos.y *
        // 100));
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
            // static const char *names[] = {"NONE", "LEFT", "RIGHT", "UP", "DOWN"};
            // LOG_INF("Gesture: %s disp=%u vel=%u", names[gesture], displacement, velocity);
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
        struct mpr121_grid_pos pos = mpr121_calc_position(touch_status, 0.5, 0.5);
        pos.x *= cfg->movement_scale;
        pos.y *= cfg->movement_scale;
        data->start_pos = pos;
        data->last_pos = pos;
        data->touch_start_time = k_uptime_get();
        data->is_touched = true;
        data->total_movement = 0;

        // data->kf_x_est = pos.x;
        // data->kf_x_cov = cfg->kf_initial_cov;
        // data->kf_y_est = pos.y;
        // data->kf_y_cov = cfg->kf_initial_cov;
        // data->kf_strided_x = true;

        // LOG_INF("Touch start: x=%d y=%d", (int)(pos.x * 100), (int)(pos.y * 100));
        raise_zmk_mpr121_touch_start_event((struct zmk_mpr121_touch_start_event){
            .x = pos.x,
            .y = pos.y,
        });
    } else if (data->is_touched && currently_touched) {
        struct mpr121_grid_pos pos =
            mpr121_calc_position(touch_status, data->last_pos.x / cfg->movement_scale,
                                 data->last_pos.y / cfg->movement_scale);
        pos.x *= cfg->movement_scale;
        pos.y *= cfg->movement_scale;
        // struct mpr121_grid_pos raw_pos =
        //     mpr121_calc_position(touch_status, data->last_pos.x / cfg->movement_scale,
        //                          data->last_pos.y / cfg->movement_scale);
        // raw_pos.x *= cfg->movement_scale;
        // raw_pos.y *= cfg->movement_scale;

        // struct mpr121_grid_pos pos;
        // if (data->kf_strided_x) {
        //     pos.x = mpr121_kf_update(&data->kf_x_est, &data->kf_x_cov, raw_pos.x,
        //                              cfg->kf_process_noise, cfg->kf_measurement_noise);
        //     pos.y = data->kf_y_est;
        // } else {
        //     pos.x = data->kf_x_est;
        //     pos.y = mpr121_kf_update(&data->kf_y_est, &data->kf_y_cov, raw_pos.y,
        //                              cfg->kf_process_noise, cfg->kf_measurement_noise);
        // }
        // data->kf_strided_x = !data->kf_strided_x;

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
        /* Still touching — continue polling at the configured interval. */
        k_work_schedule(&data->poll_work, K_MSEC(cfg->poll_interval_ms));
    } else {
        int pin_level = gpio_pin_get_dt(&data->config->interrupt_gpio);
        if (pin_level < 0) {
            /*
             * Pin read failed — something is wrong with the GPIO.  Try to
             * re-enable the interrupt anyway as a best-effort recovery.
             */
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
            /*
             * IRQ pin is already HIGH — a touch event arrived while the
             * interrupt was disabled.  Do NOT re-enable the edge interrupt
             * (the edge was already missed).  Instead, immediately start
             * polling to process the pending event.
             */
            LOG_DBG("IRQ already active, skipping interrupt, restarting poll");
            k_work_reschedule(&data->poll_work, K_MSEC(cfg->poll_interval_ms));
        } else {
            /*
             * IRQ pin is LOW (idle) — safe to re-enable the edge interrupt.
             * The next touch will produce a clean LOW->HIGH transition.
             */
            int ret =
                gpio_pin_interrupt_configure_dt(&data->config->interrupt_gpio, GPIO_INT_EDGE_BOTH);
            if (ret < 0) {
                /*
                 * Re-enable failed (e.g., GPIO driver issue).  Fall back to
                 * a short polling interval so the touchpad remains functional
                 * rather than becoming permanently unresponsive.
                 */
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
    mpr121_i2c_write(dev, MPR121_CONFIG1, 0x30);
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
        .touch_threshold = DT_INST_PROP_OR(n, touch_threshold, 3),                                 \
        .release_threshold = DT_INST_PROP_OR(n, release_threshold, 1),                             \
        .gesture_min_displacement = DT_INST_PROP_OR(n, gesture_min_displacement, 20),              \
        .gesture_min_velocity = DT_INST_PROP_OR(n, gesture_min_velocity, 200),                     \
        .gesture_max_duration_ms = DT_INST_PROP_OR(n, gesture_max_duration_ms, 300),               \
        .poll_interval_ms = DT_INST_PROP_OR(n, poll_interval_ms, 10),                              \
        .tap_max_duration_ms = DT_INST_PROP_OR(n, tap_max_duration_ms, 200),                       \
        .tap_max_displacement = DT_INST_PROP_OR(n, tap_max_displacement, 5),                       \
        .movement_scale = DT_INST_PROP_OR(n, movement_scale, 100),                                 \
        .kf_process_noise = DT_INST_PROP_OR(n, kalman_process_noise, 50) / 100.0f,                 \
        .kf_measurement_noise = DT_INST_PROP_OR(n, kalman_measurement_noise, 10) / 100.0f,         \
        .kf_initial_cov = DT_INST_PROP_OR(n, kalman_initial_cov, 100) / 100.0f,                    \
    };                                                                                             \
    DEVICE_DT_INST_DEFINE(n, mpr121_init, NULL, &mpr121_data_##n, &mpr121_cfg_##n, POST_KERNEL,    \
                          CONFIG_SENSOR_INIT_PRIORITY, NULL);

DT_INST_FOREACH_STATUS_OKAY(MPR121_INST)
