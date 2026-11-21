/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Coban Tab 1a dev harness probes (workspace backlog h01).
 *
 * Bench bring-up for the peripherals of the final board before their real
 * drivers exist (h02-h04, p01). Every probe is additionally gated on its
 * devicetree node, so this file is inert without the devkit overlay:
 *
 *   - WS2812 chase on `indicator_strip`            (h04)
 *   - key/switch event logging via input subsystem (h04)
 *   - I2S test tone on `&i2s0`                     (h03)
 *   - VBAT ADC raw read on `tab1a_vbat`            (p01)
 *
 * Pin authority: zmk/esp-port/tab1a-hardware-map.md; wiring + expectations:
 * zmk/esp-port/tab1a-bench-runbook.md.
 */

#include <zephyr/kernel.h>
#include <zephyr/input/input.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/drivers/adc.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>

#define STRIP_NODE DT_NODELABEL(indicator_strip)
#define VBAT_NODE  DT_NODELABEL(tab1a_vbat)

#define STRIP DT_NODE_EXISTS(STRIP_NODE)
#define I2S0  DT_NODE_EXISTS(DT_NODELABEL(i2s0))
#define VBAT  DT_NODE_EXISTS(VBAT_NODE)

#define HARNESS_TONE_DELAY_MS 5000

/* ------------------------------------------------------------------ */
/* WS2812 light bar chase                                             */
/* ------------------------------------------------------------------ */

#if STRIP

static const struct device *strip_dev = DEVICE_DT_GET(STRIP_NODE);
#define STRIP_LEN DT_PROP(STRIP_NODE, chain_length)

static struct k_work_delayable strip_chase_work;
static uint8_t chase_pos;

static struct led_rgb strip_pixels[STRIP_LEN];

static void strip_chase_fn(struct k_work *work) {
    for (size_t i = 0; i < STRIP_LEN; i++) {
        strip_pixels[i] = (struct led_rgb){
            .r = (uint8_t)(i == chase_pos ? 0x40 : 0x00),
            .g = (uint8_t)(i == chase_pos ? 0x08 : 0x00),
            .b = (uint8_t)(i == chase_pos ? 0x00 : 0x00),
        };
    }

    int err = led_strip_update_rgb(strip_dev, strip_pixels, STRIP_LEN);
    if (err) {
        LOG_ERR("harness: led strip update failed (%d)", err);
        return; /* stop the chase; device problem */
    }

    chase_pos = (chase_pos + 1) % STRIP_LEN;
    k_work_reschedule(&strip_chase_work, K_MSEC(120));
}

#endif /* STRIP */

/* ------------------------------------------------------------------ */
/* Key matrix events                                                   */
/* ------------------------------------------------------------------ */

static int harness_position_cb(const zmk_event_t *eh) {
    struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);
    if (ev) {
        LOG_INF("harness: key pos %u %s", ev->position, ev->state ? "pressed" : "released");
    }
    return 0;
}

ZMK_LISTENER(tab1a_harness, harness_position_cb);
ZMK_SUBSCRIPTION(tab1a_harness, zmk_position_state_changed);

/* ------------------------------------------------------------------ */
/* I2S test tone (16 kHz stereo, ~166 Hz sine, ~0.5 s)                 */
/* ------------------------------------------------------------------ */

#if I2S0

static const struct device *i2s_dev = DEVICE_DT_GET(DT_NODELABEL(i2s0));

#define TONE_RATE_HZ 16000
#define TONE_BLOCK   1024 /* stereo frames */

K_MEM_SLAB_DEFINE(tone_slab, TONE_BLOCK * 4, 2, 4);

static int16_t tone_block[TONE_BLOCK * 2];

static const int16_t tone_wave[24] = {
    0,      4240,  8192,  11584, 14187, 15826,  16383,  15826,
    14187,  11584, 8192,  4240,  0,     -4240,  -8192,  -11584,
    -14187, -15826, -16383, -15826, -14187, -11584, -8192, -4240,
};

static void i2s_tone_play(void) {
    struct i2s_config config = {
        .word_size = 16,
        .channels = 2,
        .format = I2S_FMT_DATA_FORMAT_I2S,
        .options = I2S_OPT_BIT_CLK_CONTROLLER | I2S_OPT_FRAME_CLK_CONTROLLER,
        .frame_clk_freq = TONE_RATE_HZ,
        .mem_slab = &tone_slab,
        .block_size = sizeof(tone_block),
        .timeout = 200,
    };

    /* i2s_esp32 starts its DMA from the driver's block queue, which only
     * i2s_write() fills — START before the first write fails with
     * -ENOMEM (-12) surfaced as -EIO (h01 bench 2026-11-21). Prime the
     * queue with the first two blocks, then start, then keep feeding. */
    int err = i2s_configure(i2s_dev, I2S_DIR_TX, &config);
    if (err) {
        LOG_ERR("harness: i2s configure failed (%d)", err);
        return;
    }

    int block = 0;
    for (; block < 2; block++) {
        for (int f = 0; f < TONE_BLOCK; f++) {
            int frame = block * TONE_BLOCK + f;
            int16_t s = tone_wave[frame % ARRAY_SIZE(tone_wave)];
            tone_block[f * 2] = s;
            tone_block[f * 2 + 1] = s;
        }
        err = i2s_write(i2s_dev, tone_block, sizeof(tone_block));
        if (err) {
            LOG_ERR("harness: i2s prime write failed at block %d (%d)", block, err);
            return;
        }
    }

    err = i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_START);
    if (err) {
        LOG_ERR("harness: i2s start failed (%d)", err);
        return;
    }

    for (; block < 8; block++) {
        for (int f = 0; f < TONE_BLOCK; f++) {
            int frame = block * TONE_BLOCK + f;
            int16_t s = tone_wave[frame % ARRAY_SIZE(tone_wave)];
            tone_block[f * 2] = s;
            tone_block[f * 2 + 1] = s;
        }
        err = i2s_write(i2s_dev, tone_block, sizeof(tone_block));
        if (err) {
            LOG_ERR("harness: i2s write failed at block %d (%d)", block, err);
            break;
        }
    }

    i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_STOP);
    LOG_INF("harness: i2s tone done (listen on speaker pads)");
}

#endif /* I2S0 */

/* ------------------------------------------------------------------ */
/* VBAT ADC raw read                                                   */
/* ------------------------------------------------------------------ */

#if VBAT

/* Plain adc_read on &adc1, channel 4 = GPIO5 (ADC1_GPIO5_CHANNEL). Kept
 * deliberately low-tech (no ADC_DT_SPEC — the zephyr,user binding does not
 * resolve io-channels phandles on this tree); the overlay node's
 * io-channels property documents the intent for p01. */
static const struct device *adc_dev = DEVICE_DT_GET(DT_NODELABEL(adc1));
#define VBAT_ADC_CHANNEL 4
#define VBAT_ADC_RESOLUTION 12

static void vbat_read(void) {
    if (!device_is_ready(adc_dev)) {
        LOG_ERR("harness: adc1 not ready");
        return;
    }

    uint16_t raw;
    struct adc_sequence sequence = {
        .channels = BIT(VBAT_ADC_CHANNEL),
        .buffer = &raw,
        .buffer_size = sizeof(raw),
        .resolution = VBAT_ADC_RESOLUTION,
    };

    int err = adc_read(adc_dev, &sequence);
    if (err) {
        LOG_ERR("harness: VBAT adc read failed (%d)", err);
        return;
    }

    /* Divider ratio is measured at the bench and recorded in the hardware
     * map; the harness only proves the conversion chain works. */
    LOG_INF("harness: VBAT raw %u (ch %u, res %u) — convert per hardware map", raw,
            VBAT_ADC_CHANNEL, VBAT_ADC_RESOLUTION);
}

#endif /* VBAT */

/* ------------------------------------------------------------------ */

static void harness_work_fn(struct k_work *work) {
#if STRIP
    if (device_is_ready(strip_dev)) {
        LOG_INF("harness: WS2812 chase on IO46 (%u px)", (unsigned int)STRIP_LEN);
        k_work_reschedule(&strip_chase_work, K_NO_WAIT);
    } else {
        LOG_ERR("harness: led strip not ready");
    }
#endif

#if I2S0
    if (device_is_ready(i2s_dev)) {
        LOG_INF("harness: playing i2s0 tone");
        i2s_tone_play();
    } else {
        LOG_ERR("harness: i2s0 not ready");
    }
#endif

#if VBAT
    vbat_read();
#endif

    LOG_INF("harness: probes finished; press devkit keys to see events logged");
}

K_WORK_DELAYABLE_DEFINE(harness_work, harness_work_fn);

static int tab1a_harness_init(void) {
#if STRIP
    k_work_init_delayable(&strip_chase_work, strip_chase_fn);
#endif
    k_work_schedule(&harness_work, K_MSEC(HARNESS_TONE_DELAY_MS));
    return 0;
}

SYS_INIT(tab1a_harness_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
