/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * AXS15231B integrated capacitive touch controller driver.
 *
 * The AXS15231B LCD chip integrates a capacitive touch controller with its
 * own host interface: the datasheet's "I2C/SPI in AXS15231 is the interface
 * for touch communication with HOST" is a separate slave port on the chip
 * (chip pins GPIO0=SCL, GPIO1=SDA in I2C work mode), not the display's
 * QSPI bus. On the cobantab1a module those lines reach the ESP32-S3 at
 * SDA GPIO4 / SCL GPIO8, the wiring the Guition JC3248W535 reference bsp
 * uses (its EXAMPLE_PIN_NUM_QSPI_TOUCH_SDA/SCL). There is no INT or RST
 * line wired, so the driver polls.
 *
 * Bring-up note (findings §19): an earlier verdict (§18) that this interface
 * never answers was WRONG. The bus never left the ESP32-S3: the i2c pinctrl
 * group lacked drive-open-drain, so the pads stayed push-pull with output
 * always on and the master fought every slave's ACK low - a guaranteed NACK
 * at every address, at every speed. With drive-open-drain on GPIO4/GPIO8 the
 * controller ACKs and reports touch points on the very first boot. Bench
 * 2026-11-21: 320x480 coordinates, ~4 polls/s of clean data while swiping.
 *
 * Occasional glitch frames (all 0xFF, or a shifted frame) come back from the
 * controller roughly once a second while touching; the point-count sanity
 * check below drops them.
 *
 * Read protocol (ported from the reference bsp):
 *   1. Write 11 bytes:
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

/* Mailbox-freeze watchdog: identical implausible replies in a row before a
 * recovery (~2 s at the 10 ms poll). The count is deliberately long: at
 * 100 Hz polling we read faster than the ~50 Hz touch engine writes, so
 * consecutive polls legitimately see the same live frame in pairs - only a
 * multi-second unbroken streak is a latch. Soft SWRESET tries before
 * escalating to a hard (SLPIN + SWRESET) one, and a hard cap so a truly
 * dead chip cannot make us re-init the panel forever. */
#define FREEZE_SAME_FRAMES 200
#define FREEZE_SOFT_TRIES 2
#define FREEZE_RECOVERY_MAX 4

/* Time-based press freshness (bench 2026-11-22): a live engine streams a
 * fresh valid touch point on (almost) every poll while the finger is down,
 * with roughly one glitch frame per second. A latched engine instead
 * echoes stale press/glitch frames after the finger lifts, so a press that
 * has not seen a fresh point for this long is over - release it and let
 * the gesture detectors see an edge. At the 10 ms poll that is 5 missed
 * polls: more than a single glitch frame costs (~2 polls), far shorter
 * than any real hold. */
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
    /* Mailbox-freeze watchdog (see zmk/axs15231b_recovery.h): the last raw
     * reply, how many replies in a row matched it, and recovery state. */
    uint8_t last_frame[AXS_TOUCH_DATA_LEN];
    bool have_frame;
    unsigned int same_count;
    unsigned int recoveries;
    bool recovering;
    /* Time-based press freshness: when the last fresh valid touch point
     * arrived, how many consecutive glitch replies we have seen, and how
     * many presses the freshness timer had to release. */
    int64_t last_point_ms;
    unsigned int dead_frames;
    unsigned int stale_releases;
};

INPUT_TOUCH_STRUCT_CHECK(struct axs15231b_touch_config);

static int axs15231b_touch_read(const struct device *dev, uint8_t *resp) {
    const struct axs15231b_touch_config *cfg = dev->config;
    int err;

    /* Two separate transactions with a STOP in between, exactly like the
     * vendor bsp (esp_lcd_panel_io_tx_param + rx_param). A repeated START
     * (i2c_write_read) returned err 0 too, but the controller's mailbox
     * FSM then echo-freezes (bench 2026-11-21: every reply 0x02/0x41 with
     * the frame-counter byte frozen). */
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

    /* Mailbox-freeze watchdog: with the engine latched, every reply is
     * byte-identical - the two frame-counter bytes (6, 7) included, which
     * live data never is (they tick every poll even under a motionless
     * finger, bench 2026-11-22). Two latch signatures:
     * - a frozen reply whose point-number byte is implausible (>2), and
     * - a frozen reply while we are reporting a press: the mailbox keeps
     *   echoing the last touch frame, so the num=0 release frame that
     *   would clear the press never arrives (a stuck TOUCHED, and no
     *   release edge for the swipe detectors, bench 2026-11-22).
     * Idle silence (num=0 echo, nothing pressed) is normal and not counted. */
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
        /* Glitch frame - all 0xFF (a bogus 255-point count) or byte-shifted,
         * roughly once a second while the finger moves (bench 2026-11-21).
         * The panel reports at most two points, so a bigger count is never
         * real data. Ignore the frame itself (a real press survives a single
         * glitch), but it is not a fresh touch point either: a stream of
         * them starves the freshness timer below and fills dead_frames for
         * the recovery watchdog. */
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

    /* Time-based release: whatever the latched engine echoes (frozen frame,
     * glitch storm, stale coordinates), a press without a fresh valid point
     * for PRESS_FRESH_MS is over. Without this edge the swipe detectors
     * never run their dx/dy math and the UI stays TOUCHED forever
     * (bench 2026-11-22). If the finger really is still down after the
     * engine comes back to life, the next live frame re-asserts the press. */
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

        /* Belt and braces: the freshness timer above should already have
         * released any press by now (~1.3 s before this fires). */
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

/* The panel that has to be re-initialised on a freeze, if this build has
 * one (the touch driver only exists on AXS15231B boards anyway). */
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
        /* Recovery left the GRAM black while LVGL still believes the screen
         * is painted: mark every styled object dirty so the whole UI
         * repaints on the next handler call (<= 10 ms away). */
        lv_obj_report_style_change(NULL);
    }
#endif
#else
    ARG_UNUSED(hard);
#endif

    /* Re-arm detection immediately; a still-frozen stream re-triggers
     * within one poll and escalates (soft -> hard). */
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
