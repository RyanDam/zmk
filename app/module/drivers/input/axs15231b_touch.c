/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * AXS15231B integrated capacitive touch: a separate I2C slave port on the
 * display chip (GPIO0=SCL, GPIO1=SDA), wired to ESP32-S3 GPIO8/GPIO4. No INT
 * or RST line, so this driver polls. The i2c pinctrl group needs
 * drive-open-drain, otherwise the pads stay push-pull and every address NACKs.
 *
 * Read protocol: write {0xB5,0xAB,0xA5,0x5A,0,0,0,8,0,0,0}, then read 8:
 * [0] gesture, [1] point count, [2:3] x (high nibble then low byte),
 * [4:5] y (same). Glitch frames (all 0xFF or byte-shifted) show up about
 * once a second while touching; the point-count check below drops them.
 */

#define DT_DRV_COMPAT axs_axs15231b_touch

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/input/input.h>
#include <zephyr/input/input_touch.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zmk/axs15231b_recovery.h>

#if IS_ENABLED(CONFIG_LVGL)
#include <lvgl.h>
#endif

LOG_MODULE_REGISTER(axs15231b_touch, CONFIG_INPUT_LOG_LEVEL);

/* No INT line is wired, so poll. */
#define AXS_TOUCH_POLL_PERIOD_MS 10

#define AXS_TOUCH_REQUEST_LEN 11
#define AXS_TOUCH_DATA_LEN 8

/* The panel reports at most two touch points per frame. */
#define AXS_TOUCH_MAX_POINTS 2

/* Freeze watchdog: N identical implausible replies in a row (~2 s at the
 * 10 ms poll). N is long on purpose: at 10 ms we outrun the ~50 Hz engine, so
 * consecutive polls legitimately see the same live frame in pairs. Soft SWRESET
 * first, then hard (SLPIN + SWRESET), with a cap so a dead chip cannot loop. */
#define FREEZE_SAME_FRAMES 200
#define FREEZE_SOFT_TRIES 2
#define FREEZE_RECOVERY_MAX 4

/* Press freshness: a live engine streams a fresh valid point on almost every
 * poll, a latched one echoes stale frames after the finger lifts — so a press
 * with no fresh point for PRESS_FRESH_MS is over. 5 missed polls: more than a
 * glitch costs (~2), far less than any real hold. */
#define PRESS_FRESH_MS 50

static const uint8_t axs_touch_request[AXS_TOUCH_REQUEST_LEN] = {
    0xB5,
    0xAB,
    0xA5,
    0x5A,
    0x00,
    0x00,
    (uint8_t)(AXS_TOUCH_DATA_LEN >> 8),
    (uint8_t)(AXS_TOUCH_DATA_LEN & 0xFF),
    0x00,
    0x00,
    0x00,
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
    uint8_t last_frame[AXS_TOUCH_DATA_LEN];
    bool have_frame;
    unsigned int same_count;
    unsigned int recoveries;
    bool recovering;
    int64_t last_point_ms;
    unsigned int dead_frames;
    unsigned int stale_releases;
};

INPUT_TOUCH_STRUCT_CHECK(struct axs15231b_touch_config);

static int axs15231b_touch_read(const struct device *dev, uint8_t *resp) {
    const struct axs15231b_touch_config *cfg = dev->config;
    int err;

    /* Two transactions with a STOP between, like the vendor bsp. A repeated
     * START (i2c_write_read) also reads fine but echo-freezes the mailbox FSM. */
    err = i2c_write(cfg->bus, axs_touch_request, sizeof(axs_touch_request), cfg->address);
    if (err) {
        return err;
    }
    return i2c_read(cfg->bus, resp, AXS_TOUCH_DATA_LEN, cfg->address);
}

static void axs15231b_touch_process(const struct device *dev);
static void axs15231b_touch_recover(const struct device *dev, bool hard);

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

    /* Freeze detector: when the engine latches, every reply is byte-identical
     * including the frame-counter bytes (6, 7), which live data always changes.
     * A latch is a frozen reply with an implausible point count (>2), or a
     * frozen reply while a press is reported — then the num=0 release frame
     * never arrives. Idle silence (num=0, nothing pressed) is normal. */
    if (data->have_frame && memcmp(data->last_frame, resp, sizeof(data->last_frame)) == 0 &&
        (resp[1] > AXS_TOUCH_MAX_POINTS || data->touched)) {
        data->same_count++;
    } else {
        data->same_count = 0;
    }
    memcpy(data->last_frame, resp, sizeof(data->last_frame));
    data->have_frame = true;

    if (resp[1] == 0) {
        data->dead_frames = 0;
        if (data->touched) {
            data->touched = false;

            /* Report the last position with the touch button released. */
            input_touchscreen_report_pos(dev, data->last_x, data->last_y, K_NO_WAIT);
            input_report_key(dev, INPUT_BTN_TOUCH, 0, true, K_NO_WAIT);
        }
    } else if (resp[1] > AXS_TOUCH_MAX_POINTS) {
        /* Glitch frame: all 0xFF or byte-shifted, ~1/s while the finger moves.
         * The panel reports at most two points, so a bigger count is never real.
         * Ignored, but not fresh either: a stream of them must starve the
         * freshness timer and feed dead_frames. */
        data->dead_frames++;
    } else {
        uint16_t x = ((resp[2] & 0x0F) << 8) | resp[3];
        uint16_t y = ((resp[4] & 0x0F) << 8) | resp[5];

        data->dead_frames = 0;
        data->last_point_ms = k_uptime_get();
        data->touched = true;
        data->last_x = x;
        data->last_y = y;

        input_touchscreen_report_pos(dev, x, y, K_NO_WAIT);
        input_report_key(dev, INPUT_BTN_TOUCH, 1, true, K_NO_WAIT);
    }

    /* Time-based release: a press without a fresh valid point for
     * PRESS_FRESH_MS is over, whatever the latched engine echoes. Without that
     * edge the swipe detectors never run their dx/dy math and the UI stays
     * TOUCHED; a finger still down re-asserts on the next live frame. */
    if (data->touched) {
        int64_t age = k_uptime_get() - data->last_point_ms;

        if (age > PRESS_FRESH_MS) {
            data->touched = false;
            data->stale_releases++;
            LOG_WRN("no fresh touch point for %lld ms - forced release #%u", (long long)age,
                    data->stale_releases);
            input_touchscreen_report_pos(dev, data->last_x, data->last_y, K_NO_WAIT);
            input_report_key(dev, INPUT_BTN_TOUCH, 0, true, K_NO_WAIT);
        }
    }

    if (!data->recovering &&
        (data->same_count >= FREEZE_SAME_FRAMES || data->dead_frames >= FREEZE_SAME_FRAMES)) {
        data->recovering = true;

        /* The freshness timer should have released this ~1.3 s earlier. */
        if (data->touched) {
            data->touched = false;
            input_touchscreen_report_pos(dev, data->last_x, data->last_y, K_NO_WAIT);
            input_report_key(dev, INPUT_BTN_TOUCH, 0, true, K_NO_WAIT);
        }

        if (data->recoveries < FREEZE_RECOVERY_MAX) {
            data->recoveries++;
            LOG_WRN("touch mailbox frozen (%u same, %u dead frames) - panel recovery %u/%u",
                    data->same_count, data->dead_frames, data->recoveries, FREEZE_RECOVERY_MAX);
            axs15231b_touch_recover(dev, data->recoveries > FREEZE_SOFT_TRIES);
        } else {
            LOG_ERR("touch mailbox still frozen after %u recoveries - giving up (display "
                    "refresh would still recover it)",
                    FREEZE_RECOVERY_MAX);
        }
    }
}

static void axs15231b_touch_work_handler(struct k_work *work) {
    struct axs15231b_touch_data *data = CONTAINER_OF(work, struct axs15231b_touch_data, work);

    axs15231b_touch_process(data->dev);
}

/* Panel to re-initialise on a freeze, if this build has one. */
#if IS_ENABLED(CONFIG_AXS15231B)
#define AXS15231B_DISPLAY_DEV DEVICE_DT_GET_OR_NULL(DT_INST(0, axs_axs15231b))
#else
#define AXS15231B_DISPLAY_DEV NULL
#endif

static void axs15231b_touch_recover(const struct device *dev, bool hard) {
    struct axs15231b_touch_data *data = dev->data;
#if IS_ENABLED(CONFIG_AXS15231B)
    const struct device *panel = AXS15231B_DISPLAY_DEV;
    int err;

    err = panel ? zmk_axs15231b_recover(panel, hard) : -ENODEV;
    if (err) {
        LOG_ERR("panel recovery failed: %d", err);
    }
#if IS_ENABLED(CONFIG_LVGL)
    else {
        /* Recovery left the GRAM black while LVGL still believes it is painted:
         * dirty every styled object so the UI repaints within 10 ms. */
        lv_obj_report_style_change(NULL);
    }
#endif
#else
    ARG_UNUSED(hard);
#endif

    /* Re-arm now; a still-frozen stream re-triggers within a poll and escalates. */
    data->same_count = 0;
    data->recovering = false;
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
    k_timer_start(&data->timer, K_MSEC(AXS_TOUCH_POLL_PERIOD_MS), K_MSEC(AXS_TOUCH_POLL_PERIOD_MS));

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

#define AXS15231B_TOUCH_DATA (&axs15231b_touch_data_0)

void zmk_axs15231b_touch_status(bool *frozen, unsigned int *same, unsigned int *stale,
                                unsigned int *recoveries, uint8_t frame[8]) {
    const struct axs15231b_touch_data *data = AXS15231B_TOUCH_DATA;

    *frozen = data->same_count >= FREEZE_SAME_FRAMES || data->dead_frames >= FREEZE_SAME_FRAMES;
    *same = data->same_count;
    *stale = data->stale_releases;
    *recoveries = data->recoveries;
    if (frame != NULL) {
        memcpy(frame, data->last_frame, sizeof(data->last_frame));
    }
}
