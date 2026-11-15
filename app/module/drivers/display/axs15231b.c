/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * AXS15231B QSPI LCD panel driver.
 *
 * The AXS15231B is a 320x480 (and other resolutions) LCD controller with a
 * QSPI interface. It is used on the Guition JC3248W535 reference board and the
 * cobantab1a module. The panel has no DC/RS or RST pin wired on those boards,
 * so it is configured for QSPI-only operation and reset via the SWRESET
 * command.
 *
 * QSPI wire protocol (verified against Espressif's esp_lcd_panel_io_spi.c and
 * the JC3248W535 reference bsp):
 *   - Command phase: a 32-bit opcode sent in 1-wire mode (D0 only):
 *       [opcode][cmd][0x00][0x00]
 *     where opcode is 0x02 (write command) or 0x32 (write color).
 *   - Parameter phase: sent in 1-wire mode (D0 only), same CS transaction.
 *   - Color data phase: sent in 4-wire (QIO) mode, CS held from the command.
 *
 * The command and its parameters share one CS assertion; the color command and
 * its pixel data share one CS assertion (the line mode switches from 1-wire to
 * 4-wire mid-transaction, with CS held).
 */

#define DT_DRV_COMPAT axs_axs15231b

#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>
#include <soc/gpio_reg.h>

LOG_MODULE_REGISTER(axs15231b, CONFIG_DISPLAY_LOG_LEVEL);

#define AXS_WIDTH DT_INST_PROP(0, width)
#define AXS_HEIGHT DT_INST_PROP(0, height)

#define AXS_HAS_BACKLIGHT DT_NODE_HAS_PROP(DT_INST(0, axs_axs15231b), pwms)

/* TEMPORARY diagnostic: fill the screen with color bands at init to verify
 * the pixel-write (RAMWR) path reaches the panel GRAM. Remove once working. */
#define AXS_DIAG_FILL 1

/* QSPI opcodes (top byte of the 32-bit command word) */
#define AXS_OPCODE_WRITE_CMD 0x02
#define AXS_OPCODE_WRITE_COLOR 0x32

/* LCD controller commands */
#define AXS_CMD_SWRESET 0x01
#define AXS_CMD_SLPIN 0x10
#define AXS_CMD_SLPOUT 0x11
#define AXS_CMD_DISPOFF 0x28
#define AXS_CMD_DISPON 0x29
#define AXS_CMD_CASET 0x2A
#define AXS_CMD_RASET 0x2B
#define AXS_CMD_RAMWR 0x2C
#define AXS_CMD_RAMWRC 0x3C /* RAM write continue (used for every window after the first) */
#define AXS_CMD_MADCTL 0x36
#define AXS_CMD_COLMOD 0x3A

/* Delays (ms) from the reference bsp / panel datasheet */
#define AXS_SWRESET_DELAY_MS 120
#define AXS_SLPOUT_DELAY_MS 120
#define AXS_DISPON_DELAY_MS 20

struct axs15231b_cfg {
    struct spi_dt_spec spi;
    struct gpio_dt_spec reset;
#if AXS_HAS_BACKLIGHT
    struct pwm_dt_spec backlight;
#endif
};

struct axs15231b_data {
    /* Base SPI operation flags (master, 8-bit words, mode from DT, 1-wire). */
    uint32_t base_op;
    bool initialized;
};

/*
 * Send a 32-bit command opcode followed by optional parameters, all in 1-wire
 * mode, in a single CS transaction.
 */
static int axs15231b_tx_cmd(const struct device *dev, uint8_t cmd, const void *params,
                            size_t params_len) {
    const struct axs15231b_cfg *cfg = dev->config;
    struct axs15231b_data *data = dev->data;

    /*
     * 32-bit QSPI command word, on the wire MSB-first:
     *   [opcode][0x00][cmd][0x00]
     * The opcode (0x02) is the top byte and the LCD command is the third byte.
     * (Matches the reference bsp, which builds the word as
     * (opcode << 24) | (cmd << 8) and byte-reverses it for the big-endian LCD.)
     */
    uint8_t header[4] = {
        AXS_OPCODE_WRITE_CMD,
        0x00,
        cmd,
        0x00,
    };
    struct spi_buf bufs[2] = {
        [0] = {.buf = header, .len = sizeof(header)},
        [1] = {.buf = (void *)params, .len = params_len},
    };
    struct spi_buf_set tx = {
        .buffers = bufs,
        .count = (params_len > 0) ? 2 : 1,
    };

    struct spi_config spi_cfg = cfg->spi.config;
    spi_cfg.operation = data->base_op;

    return spi_transceive(cfg->spi.bus, &spi_cfg, &tx, NULL);
}

/*
 * Send a 32-bit color command opcode (1-wire, CS held) followed by the pixel
 * buffer (4-wire QIO), in one CS transaction. The pixel buffer may be large;
 * the SPI driver chunks it while holding CS.
 */
static int axs15231b_tx_color(const struct device *dev, uint8_t cmd, const void *pixels,
                              size_t pixels_len) {
    const struct axs15231b_cfg *cfg = dev->config;
    struct axs15231b_data *data = dev->data;
    int err;

    /* Same 32-bit word layout as the command phase: [opcode][0x00][cmd][0x00]. */
    uint8_t header[4] = {
        AXS_OPCODE_WRITE_COLOR,
        0x00,
        cmd,
        0x00,
    };

    /* Color command: 1-wire, keep CS asserted for the data that follows. */
    struct spi_config cmd_cfg = cfg->spi.config;
    cmd_cfg.operation = data->base_op | SPI_HOLD_ON_CS;

    struct spi_buf cmd_buf = {.buf = header, .len = sizeof(header)};
    struct spi_buf_set cmd_set = {.buffers = &cmd_buf, .count = 1};

    err = spi_transceive(cfg->spi.bus, &cmd_cfg, &cmd_set, NULL);
    if (err) {
        return err;
    }

    /* Pixel data: 4-wire (QIO). CS is still asserted from the command. */
    struct spi_config data_cfg = cfg->spi.config;
    data_cfg.operation = data->base_op | SPI_LINES_QUAD;

    struct spi_buf data_buf = {.buf = (void *)pixels, .len = pixels_len};
    struct spi_buf_set data_set = {.buffers = &data_buf, .count = 1};

    return spi_transceive(cfg->spi.bus, &data_cfg, &data_set, NULL);
}

#if AXS_HAS_BACKLIGHT
static int axs15231b_set_backlight(const struct device *dev, bool on) {
    const struct axs15231b_cfg *cfg = dev->config;

    return pwm_set_dt(&cfg->backlight, cfg->backlight.period, on ? cfg->backlight.period : 0);
}
#else
static inline int axs15231b_set_backlight(const struct device *dev, bool on) {
    ARG_UNUSED(dev);
    ARG_UNUSED(on);
    return 0;
}
#endif

struct axs15231b_init_cmd {
    uint8_t cmd;
    const uint8_t *params;
    uint8_t len;
    uint16_t delay_ms;
};

/*
 * Panel initialization sequence, ported verbatim from the JC3248W535 reference
 * bsp (esp_bsp.c lcd_init_cmds). This sequence is panel-tuned and differs from
 * the Espressif driver's built-in default, so it must be used as-is.
 */
static const struct axs15231b_init_cmd axs15231b_init_cmds[] = {
    {0xBB, (const uint8_t[]){0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x5A, 0xA5}, 8, 0},
    {0xA0,
     (const uint8_t[]){0xC0, 0x10, 0x00, 0x02, 0x00, 0x00, 0x04, 0x3F, 0x20, 0x05, 0x3F, 0x3F, 0x00,
                       0x00, 0x00, 0x00, 0x00},
     17, 0},
    {0xA2, (const uint8_t[]){0x30, 0x3C, 0x24, 0x14, 0xD0, 0x20, 0xFF, 0xE0, 0x40, 0x19, 0x80,
                             0x80, 0x80, 0x20, 0xF9, 0x10, 0x02, 0xFF, 0xFF, 0xF0, 0x90, 0x01,
                             0x32, 0xA0, 0x91, 0xE0, 0x20, 0x7F, 0xFF, 0x00, 0x5A},
     31, 0},
    {0xD0, (const uint8_t[]){0xE0, 0x40, 0x51, 0x24, 0x08, 0x05, 0x10, 0x01, 0x20, 0x15,
                             0x42, 0xC2, 0x22, 0x22, 0xAA, 0x03, 0x10, 0x12, 0x60, 0x14,
                             0x1E, 0x51, 0x15, 0x00, 0x8A, 0x20, 0x00, 0x03, 0x3A, 0x12},
     30, 0},
    {0xA3, (const uint8_t[]){0xA0, 0x06, 0xAA, 0x00, 0x08, 0x02, 0x0A, 0x04, 0x04, 0x04, 0x04,
                             0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x55, 0x55},
     22, 0},
    {0xC1, (const uint8_t[]){0x31, 0x04, 0x02, 0x02, 0x71, 0x05, 0x24, 0x55, 0x02, 0x00,
                             0x41, 0x00, 0x53, 0xFF, 0xFF, 0xFF, 0x4F, 0x52, 0x00, 0x4F,
                             0x52, 0x00, 0x45, 0x3B, 0x0B, 0x02, 0x0D, 0x00, 0xFF, 0x40},
     30, 0},
    {0xC3, (const uint8_t[]){0x00, 0x00, 0x00, 0x50, 0x03, 0x00, 0x00, 0x00, 0x01, 0x80, 0x01}, 11,
     0},
    {0xC4, (const uint8_t[]){0x00, 0x24, 0x33, 0x80, 0x00, 0xEA, 0x64, 0x32, 0xC8, 0x64,
                             0xC8, 0x32, 0x90, 0x90, 0x11, 0x06, 0xDC, 0xFA, 0x00, 0x00,
                             0x80, 0xFE, 0x10, 0x10, 0x00, 0x0A, 0x0A, 0x44, 0x50},
     29, 0},
    {0xC5, (const uint8_t[]){0x18, 0x00, 0x00, 0x03, 0xFE, 0x3A, 0x4A, 0x20, 0x30, 0x10, 0x88, 0xDE,
                             0x0D, 0x08, 0x0F, 0x0F, 0x01, 0x3A, 0x4A, 0x20, 0x10, 0x10, 0x00},
     23, 0},
    {0xC6, (const uint8_t[]){0x05, 0x0A, 0x05, 0x0A, 0x00, 0xE0, 0x2E, 0x0B, 0x12, 0x22,
                             0x12, 0x22, 0x01, 0x03, 0x00, 0x3F, 0x6A, 0x18, 0xC8, 0x22},
     20, 0},
    {0xC7, (const uint8_t[]){0x50, 0x32, 0x28, 0x00, 0xA2, 0x80, 0x8F, 0x00, 0x80, 0xFF,
                             0x07, 0x11, 0x9C, 0x67, 0xFF, 0x24, 0x0C, 0x0D, 0x0E, 0x0F},
     20, 0},
    {0xC9, (const uint8_t[]){0x33, 0x44, 0x44, 0x01}, 4, 0},
    {0xCF, (const uint8_t[]){0x2C, 0x1E, 0x88, 0x58, 0x13, 0x18, 0x56, 0x18, 0x1E,
                             0x68, 0x88, 0x00, 0x65, 0x09, 0x22, 0xC4, 0x0C, 0x77,
                             0x22, 0x44, 0xAA, 0x55, 0x08, 0x08, 0x12, 0xA0, 0x08},
     27, 0},
    {0xD5, (const uint8_t[]){0x40, 0x8E, 0x8D, 0x01, 0x35, 0x04, 0x92, 0x74, 0x04, 0x92,
                             0x74, 0x04, 0x08, 0x6A, 0x04, 0x46, 0x03, 0x03, 0x03, 0x03,
                             0x82, 0x01, 0x03, 0x00, 0xE0, 0x51, 0xA1, 0x00, 0x00, 0x00},
     30, 0},
    {0xD6, (const uint8_t[]){0x10, 0x32, 0x54, 0x76, 0x98, 0xBA, 0xDC, 0xFE, 0x93, 0x00,
                             0x01, 0x83, 0x07, 0x07, 0x00, 0x07, 0x07, 0x00, 0x03, 0x03,
                             0x03, 0x03, 0x03, 0x03, 0x00, 0x84, 0x00, 0x20, 0x01, 0x00},
     30, 0},
    {0xD7,
     (const uint8_t[]){0x03, 0x01, 0x0B, 0x09, 0x0F, 0x0D, 0x1E, 0x1F, 0x18, 0x1D, 0x1F, 0x19, 0x40,
                       0x8E, 0x04, 0x00, 0x20, 0xA0, 0x1F},
     19, 0},
    {0xD8,
     (const uint8_t[]){0x02, 0x00, 0x0A, 0x08, 0x0E, 0x0C, 0x1E, 0x1F, 0x18, 0x1D, 0x1F, 0x19}, 12,
     0},
    {0xD9,
     (const uint8_t[]){0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F}, 12,
     0},
    {0xDD,
     (const uint8_t[]){0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F}, 12,
     0},
    {0xDF, (const uint8_t[]){0x44, 0x73, 0x4B, 0x69, 0x00, 0x0A, 0x02, 0x90}, 8, 0},
    {0xE0,
     (const uint8_t[]){0x3B, 0x28, 0x10, 0x16, 0x0C, 0x06, 0x11, 0x28, 0x5C, 0x21, 0x0D, 0x35, 0x13,
                       0x2C, 0x33, 0x28, 0x0D},
     17, 0},
    {0xE1,
     (const uint8_t[]){0x37, 0x28, 0x10, 0x16, 0x0B, 0x06, 0x11, 0x28, 0x5C, 0x21, 0x0D, 0x35, 0x14,
                       0x2C, 0x33, 0x28, 0x0F},
     17, 0},
    {0xE2,
     (const uint8_t[]){0x3B, 0x07, 0x12, 0x18, 0x0E, 0x0D, 0x17, 0x35, 0x44, 0x32, 0x0C, 0x14, 0x14,
                       0x36, 0x3A, 0x2F, 0x0D},
     17, 0},
    {0xE3,
     (const uint8_t[]){0x37, 0x07, 0x12, 0x18, 0x0E, 0x0D, 0x17, 0x35, 0x44, 0x32, 0x0C, 0x14, 0x14,
                       0x36, 0x32, 0x2F, 0x0F},
     17, 0},
    {0xE4,
     (const uint8_t[]){0x3B, 0x07, 0x12, 0x18, 0x0E, 0x0D, 0x17, 0x39, 0x44, 0x2E, 0x0C, 0x14, 0x14,
                       0x36, 0x3A, 0x2F, 0x0D},
     17, 0},
    {0xE5,
     (const uint8_t[]){0x37, 0x07, 0x12, 0x18, 0x0E, 0x0D, 0x17, 0x39, 0x44, 0x2E, 0x0C, 0x14, 0x14,
                       0x36, 0x3A, 0x2F, 0x0F},
     17, 0},
    {0xA4,
     (const uint8_t[]){0x85, 0x85, 0x95, 0x82, 0xAF, 0xAA, 0xAA, 0x80, 0x10, 0x30, 0x40, 0x40, 0x20,
                       0xFF, 0x60, 0x30},
     16, 0},
    {0xA4, (const uint8_t[]){0x85, 0x85, 0x95, 0x85}, 4, 0},
    {0xBB, (const uint8_t[]){0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, 8, 0},
    {0x13, NULL, 0, 0},                   /* SLPIN */
    {0x11, NULL, 0, AXS_SLPOUT_DELAY_MS}, /* SLPOUT */
    {0x2C, (const uint8_t[]){0x00, 0x00, 0x00, 0x00}, 4, 0},
};

#if AXS_DIAG_FILL
static void axs15231b_diag_fill(const struct device *dev);
#endif

static int axs15231b_controller_init(const struct device *dev) {
    struct axs15231b_data *data = dev->data;
    const struct axs15231b_cfg *cfg = dev->config;
    int err;

    LOG_DBG("Initializing AXS15231B controller");

    /* Optional hardware reset (not wired on the JC3248W535 / cobantab1a). */
    if (cfg->reset.port != NULL) {
        gpio_pin_set_dt(&cfg->reset, 1);
        k_msleep(10);
        gpio_pin_set_dt(&cfg->reset, 0);
        k_msleep(10);
        gpio_pin_set_dt(&cfg->reset, 1);
        k_msleep(AXS_SWRESET_DELAY_MS);
    } else {
        /* Software reset. */
        err = axs15231b_tx_cmd(dev, AXS_CMD_SWRESET, NULL, 0);
        if (err) {
            LOG_ERR("SWRESET failed: %d", err);
            return err;
        }
        k_msleep(AXS_SWRESET_DELAY_MS);
    }

    /* Run the panel-tuned initialization sequence. */
    for (size_t i = 0; i < ARRAY_SIZE(axs15231b_init_cmds); i++) {
        const struct axs15231b_init_cmd *c = &axs15231b_init_cmds[i];

        err = axs15231b_tx_cmd(dev, c->cmd, c->params, c->len);
        if (err) {
            LOG_ERR("Init cmd 0x%02X failed: %d", c->cmd, err);
            return err;
        }
        if (c->delay_ms) {
            k_msleep(c->delay_ms);
        }
    }

    /*
     * The reference sequence ends with SLPIN/SLPOUT and a RAMWR of 4 zero
     * bytes. Set the display orientation and pixel format, then turn the
     * display on.
     */
    uint8_t madctl = 0x00; /* no swap, no mirror */
    err = axs15231b_tx_cmd(dev, AXS_CMD_MADCTL, &madctl, 1);
    if (err) {
        return err;
    }

    uint8_t colmod = 0x55; /* 16-bit/pixel RGB565 */
    err = axs15231b_tx_cmd(dev, AXS_CMD_COLMOD, &colmod, 1);
    if (err) {
        return err;
    }

    err = axs15231b_tx_cmd(dev, AXS_CMD_DISPON, NULL, 0);
    if (err) {
        return err;
    }
    k_msleep(AXS_DISPON_DELAY_MS);

    data->initialized = true;

#if AXS_DIAG_FILL
    axs15231b_diag_fill(dev);
#endif

    LOG_INF("AXS15231B initialized (%ux%u)", AXS_WIDTH, AXS_HEIGHT);
    return 0;
}

static int axs15231b_write(const struct device *dev, const uint16_t x, const uint16_t y,
                           const struct display_buffer_descriptor *desc, const void *buf) {
    struct axs15231b_data *data = dev->data;
    int err;

    if (!data->initialized) {
        return -ENODEV;
    }

    uint16_t x_end = x + desc->width - 1;
    uint16_t y_end = y + desc->height - 1;

    if (x_end >= AXS_WIDTH || y_end >= AXS_HEIGHT) {
        LOG_ERR("Write out of bounds: x %u..%u, y %u..%u", x, x_end, y, y_end);
        return -EINVAL;
    }

    /*
     * Define the column (X) window. In QSPI mode the AXS15231B takes CASET
     * only — the reference driver never sends RASET, and sending it breaks the
     * window the panel uses for the write. The rows are implied by the write
     * command plus the number of pixels streamed.
     */
    uint8_t caset[4] = {
        (uint8_t)(x >> 8),
        (uint8_t)(x & 0xFF),
        (uint8_t)(x_end >> 8),
        (uint8_t)(x_end & 0xFF),
    };
    err = axs15231b_tx_cmd(dev, AXS_CMD_CASET, caset, sizeof(caset));
    if (err) {
        return err;
    }

    /*
     * RAMWR (0x2C) opens a new write; RAMWRC (0x3C, "continue writing") is used
     * for every window after the first, which is what makes partial flushes land
     * at the right rows.
     */
    const uint8_t ram_cmd = (y == 0) ? AXS_CMD_RAMWR : AXS_CMD_RAMWRC;

    /*
     * The panel's RAMWR expects a contiguous stream of pixels. The Zephyr
     * display API passes desc->pitch in pixels, which can exceed
     * desc->width (LVGL aligns the stride). If the buffer is pitched, copy
     * the window into a contiguous temporary buffer; otherwise stream it
     * directly.
     */
    uint32_t total_bytes = (uint32_t)desc->width * desc->height * 2;

    if (desc->pitch == desc->width) {
        return axs15231b_tx_color(dev, ram_cmd, buf, total_bytes);
    }

    uint8_t *tmp = k_malloc(total_bytes);
    if (tmp == NULL) {
        LOG_ERR("No memory for %u byte frame buffer", total_bytes);
        return -ENOMEM;
    }

    const uint8_t *src = buf;
    for (uint16_t row = 0; row < desc->height; row++) {
        memcpy(tmp + (uint32_t)row * desc->width * 2, src + (uint32_t)row * desc->pitch * 2,
               (uint32_t)desc->width * 2);
    }

    err = axs15231b_tx_color(dev, ram_cmd, tmp, total_bytes);
    k_free(tmp);
    return err;
}

#if AXS_DIAG_FILL
/*
 * TEMPORARY diagnostic: two observable phases run while LVGL is blocked, so what
 * the screen shows is exactly what the driver sent (nothing can override it).
 *
 * Phase 1 (8 s)  — whole panel filled solid red: exercises the 4-wire pixel
 *   path (RAMWR for the first window, RAMWRC for the rest).
 *   Screen turns RED -> pixel writes reach the panel.
 * Phase 2 (12 s) — DISPOFF, so the screen should go DARK.
 *   Screen goes DARK -> the 1-wire command path reaches the panel.
 *
 * If neither phase is visible, nothing reaches the panel at all.
 */
#define AXS_DIAG_ROWS 40

#define AXS_DIAG_MS 20000

#define DIAG_SPI2 0x60024000

/*
 * pinctrl routes these pads to the SPI2 SIGMAP signals. gpio_pin_configure
 * switches a pad to the plain GPIO latch, which shows up in its SIGMAP entry as
 * out_sel = SIG_GPIO_OUT_IDX (256). If the entry does not change, the GPIO latch
 * cannot reach the pad and a hand-driven frame never leaves the chip.
 */
static const struct device *bb_g0, *bb_g1;

static const uint32_t bb_pads[6] = {45, 47, 21, 48, 40, 39}; /* CS, PCLK, D0, D1, D2, D3 */

static const struct device *bb_port(uint32_t pad) {
    return pad >= 32 ? bb_g1 : bb_g0;
}

static uint32_t bb_pin(uint32_t pad) {
    return pad >= 32 ? pad - 32 : pad;
}

static uint8_t diag_buf[AXS_WIDTH * AXS_DIAG_ROWS * 2];

static void diag_fill_buf(uint16_t color565) {
    for (size_t i = 0; i < sizeof(diag_buf); i += 2) {
        diag_buf[i] = (uint8_t)(color565 >> 8);
        diag_buf[i + 1] = (uint8_t)(color565 & 0xff);
    }
}

static void diag_sigmap(const char *tag, uint32_t pad) {
    uint32_t v = REG_READ(GPIO_FUNC0_OUT_SEL_CFG_REG + pad * 4);

    LOG_INF("DIAG: %s pad %u out_sel=%u", tag, pad, v & 0x1FF);
}

#define DIAG_IO_MUX 0x60007004 /* IO_MUX_GPIO0_REG on the ESP32-S3 */

/*
 * Is the pad actually ours? gpio_ll_func_sel() sets pin_function = 1 (GPIO) and
 * the SIGMAP entry selects the GPIO latch (out_sel = 256). If a pad is still
 * pinned to a peripheral function, the GPIO latch cannot drive it and a
 * hand-driven frame never leaves the chip. Driving the pad and reading the input
 * register is the end-to-end check, because the input path reads the pad itself.
 */
static void diag_pad(uint32_t pad) {
    const struct device *port = bb_port(pad);
    uint32_t p = pad >= 32 ? pad - 32 : pad;
    int err = gpio_pin_configure(port, p, GPIO_OUTPUT_INACTIVE | GPIO_INPUT);

    LOG_INF("DIAG pad %u: pin_function=%u out_sel=%u out_en=%u", pad,
            REG_READ(DIAG_IO_MUX + pad * 4) & 0xF,
            REG_READ(GPIO_FUNC0_OUT_SEL_CFG_REG + pad * 4) & 0x1FF,
            (REG_READ(pad >= 32 ? GPIO_ENABLE1_REG : GPIO_ENABLE_REG) >> p) & 1);

    if (err) {
        LOG_ERR("DIAG pad %u: configure err=%d", pad, err);
        return;
    }

    uint32_t in_reg = pad >= 32 ? GPIO_IN1_REG : GPIO_IN_REG;

    gpio_pin_set(port, p, 1);
    k_busy_wait(100);
    uint32_t hi = (REG_READ(in_reg) >> p) & 1;

    gpio_pin_set(port, p, 0);
    k_busy_wait(100);
    uint32_t lo = (REG_READ(in_reg) >> p) & 1;

    LOG_INF("DIAG pad %u: driven high reads %u, driven low reads %u", pad, hi, lo);
}

/* An address answering on the touch bus proves the module is powered and
 * reachable on an interface that has nothing to do with the QSPI pads. */
static void diag_i2c_scan(void) {
    const struct device *bus = DEVICE_DT_GET(DT_NODELABEL(i2c0));
    uint8_t byte = 0;

    if (!device_is_ready(bus)) {
        LOG_ERR("DIAG: i2c0 is not ready");
        return;
    }

    LOG_INF("DIAG: I2C scan on i2c0 (SDA 4, SCL 8):");
    for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
        if (i2c_read(bus, &byte, 1, addr) == 0) {
            LOG_INF("DIAG:   addr 0x%02x answers", addr);
        }
    }
}

static void bb_set(uint32_t pad, int val) {
    gpio_pin_set(bb_port(pad), bb_pin(pad), val);
}

static void bb_edge(void) {
    k_busy_wait(10);
    bb_set(bb_pads[1], 0);
    k_busy_wait(10);
    bb_set(bb_pads[1], 1); /* rising edge = sample edge for mode 3 */
    k_busy_wait(10);
}

/*
 * Two framings, and only one of them can be what the panel wants:
 *   quad   - one clock edge per nibble, data line j carries that nibble's bit;
 *   1-wire - one clock edge per bit, only D0 is driven.
 * A byte framed one way cannot be read by a panel expecting the other, so each
 * test band below is filled with a different color using a different framing:
 * whatever color appears on screen identifies which framing the panel decoded.
 */
static void bb_byte(uint8_t byte, int quad) {
    if (quad) {
        for (int nib = 0; nib < 2; nib++) {
            for (int j = 0; j < 4; j++) {
                bb_set(bb_pads[2 + j], (byte >> (7 - nib * 4 - j)) & 1);
            }

            bb_edge();
        }

        return;
    }

    for (int bit = 7; bit >= 0; bit--) {
        for (int j = 1; j < 4; j++) {
            bb_set(bb_pads[2 + j], 0);
        }

        bb_set(bb_pads[2], (byte >> bit) & 1);
        bb_edge();
    }
}

static int bb_setup(void) {
    for (int i = 0; i < 6; i++) {
        int err = gpio_pin_configure(bb_port(bb_pads[i]), bb_pin(bb_pads[i]), GPIO_OUTPUT_INACTIVE);

        if (err) {
            LOG_ERR("DIAG: pad %u configure err=%d", bb_pads[i], err);
            return err;
        }
    }

    /* Did the GPIO latch take the pad over from pinctrl? */
    diag_sigmap("after gpio_pin_configure", bb_pads[2]);

    return 0;
}

static void bb_frame(int hdr_quad, int param_quad, uint8_t opcode, uint8_t cmd,
                     const uint8_t *params, size_t len) {
    uint8_t hdr[4] = {opcode, 0x00, cmd, 0x00};

    bb_set(bb_pads[0], 0); /* CS active */
    k_busy_wait(200);
    bb_set(bb_pads[1], 1); /* clock idles high (mode 3) */
    k_busy_wait(200);

    for (int i = 0; i < 4; i++) {
        bb_byte(hdr[i], hdr_quad);
    }

    for (size_t i = 0; i < len; i++) {
        bb_byte(params[i], param_quad);
    }

    bb_set(bb_pads[0], 1); /* CS inactive */
    k_busy_wait(200);
}

static void bb_band(int hdr_quad, int param_quad, uint16_t y0) {
    uint8_t win[4] = {0x00, 0x00, (uint8_t)((AXS_WIDTH - 1) >> 8), (uint8_t)((AXS_WIDTH - 1) & 0xff)};

    bb_frame(hdr_quad, param_quad, AXS_OPCODE_WRITE_CMD, AXS_CMD_CASET, win, 4);

    win[0] = (uint8_t)(y0 >> 8);
    win[1] = (uint8_t)(y0 & 0xff);
    win[2] = (uint8_t)((y0 + AXS_DIAG_ROWS - 1) >> 8);
    win[3] = (uint8_t)((y0 + AXS_DIAG_ROWS - 1) & 0xff);
    bb_frame(hdr_quad, param_quad, AXS_OPCODE_WRITE_CMD, AXS_CMD_RASET, win, 4);

    bb_frame(hdr_quad, param_quad, AXS_OPCODE_WRITE_COLOR, AXS_CMD_RAMWR, diag_buf, sizeof(diag_buf));
}

static void axs15231b_diag_fill(const struct device *dev) {
    struct display_buffer_descriptor desc = {
        .buf_size = sizeof(diag_buf),
        .width = AXS_WIDTH,
        .height = AXS_DIAG_ROWS,
        .pitch = AXS_WIDTH,
    };
    int err;

    bb_g0 = DEVICE_DT_GET(DT_NODELABEL(gpio0));
    bb_g1 = DEVICE_DT_GET(DT_NODELABEL(gpio1));

    LOG_INF("DIAG: pads as routed by pinctrl (out_sel = the SIGMAP signal):");
    for (int i = 0; i < 6; i++) {
        diag_sigmap("  pinctrl", bb_pads[i]);
    }

    /*
     * Band 1, rows 0-39, CYAN: through the SPI2 peripheral, everything 1-wire,
     * exactly as the driver does it today, now at 1 MHz so clock/data skew cannot
     * be the reason. This is the path LVGL uses, so it has to run before the pads
     * are switched to the GPIO latch.
     */
    err = axs15231b_tx_cmd(dev, AXS_CMD_DISPOFF, NULL, 0);
    LOG_INF("DIAG band 1 CYAN: peripheral DISPOFF err=%d", err);

    uint32_t user = REG_READ(DIAG_SPI2 + 0x10);

    LOG_INF("DIAG:   CTRL=0x%08x CLOCK=0x%08x USER=0x%08x USER1=0x%08x USER2=0x%08x MS_DLEN=0x%08x",
            REG_READ(DIAG_SPI2 + 0x08), REG_READ(DIAG_SPI2 + 0x0C), user,
            REG_READ(DIAG_SPI2 + 0x14), REG_READ(DIAG_SPI2 + 0x18), REG_READ(DIAG_SPI2 + 0x1C));
    LOG_INF("DIAG:   MISC=0x%08x DIN_MODE=0x%08x DOUT_MODE=0x%08x DMA_CONF=0x%08x W0=0x%08x",
            REG_READ(DIAG_SPI2 + 0x20), REG_READ(DIAG_SPI2 + 0x24), REG_READ(DIAG_SPI2 + 0x2C),
            REG_READ(DIAG_SPI2 + 0x30), REG_READ(DIAG_SPI2 + 0x98));

    err = axs15231b_tx_cmd(dev, AXS_CMD_DISPON, NULL, 0);
    LOG_INF("DIAG: peripheral DISPON err=%d", err);

    diag_fill_buf(0x07FF); /* CYAN */
    err = axs15231b_write(dev, 0, 0, &desc, diag_buf);
    LOG_INF("DIAG band 1 CYAN rows 0-39 via the peripheral, err=%d -> look 20 s", err);
    k_msleep(AXS_DIAG_MS);

    /*
     * Is the hardware path even able to reach the panel? Check each pad, then the
     * touch bus, which is a completely independent interface on the same module.
     */
    for (int i = 0; i < 6; i++) {
        diag_pad(bb_pads[i]);
    }

    diag_i2c_scan();

    if (bb_setup()) {
        return;
    }

    /* Init table with both framings, so whichever the panel accepts initializes
     * it, then SLPOUT + DISPON both ways as well. */
    for (int quad = 0; quad < 2; quad++) {
        for (size_t i = 0; i < ARRAY_SIZE(axs15231b_init_cmds); i++) {
            const struct axs15231b_init_cmd *c = &axs15231b_init_cmds[i];

            bb_frame(quad, quad, AXS_OPCODE_WRITE_CMD, c->cmd, c->params, c->len);
            if (c->delay_ms) {
                k_msleep(c->delay_ms);
            }
        }

        bb_frame(quad, quad, AXS_OPCODE_WRITE_CMD, AXS_CMD_SLPOUT, NULL, 0);
        k_msleep(AXS_SLPOUT_DELAY_MS);
        bb_frame(quad, quad, AXS_OPCODE_WRITE_CMD, AXS_CMD_DISPON, NULL, 0);
        k_msleep(AXS_DISPON_DELAY_MS);
    }

    diag_fill_buf(0xF800); /* RED */
    bb_band(0, 1, 40); /* 1-wire command + 4-wire data = the reference framing */
    LOG_INF("DIAG band 2 RED rows 40-79: 1-wire commands + 4-wire pixels -> look 20 s");
    k_msleep(AXS_DIAG_MS);

    diag_fill_buf(0x07E0); /* GREEN */
    bb_band(1, 1, 80);
    LOG_INF("DIAG band 3 GREEN rows 80-119: everything 4-wire -> look 20 s");
    k_msleep(AXS_DIAG_MS);

    diag_fill_buf(0x001F); /* BLUE */
    bb_band(0, 0, 120);
    LOG_INF("DIAG band 4 BLUE rows 120-159: everything 1-wire -> look 20 s");
    k_msleep(AXS_DIAG_MS);

    LOG_INF("DIAG: done. Which of CYAN/RED/GREEN/BLUE appeared, and in which rows?");
}
#endif

static int axs15231b_blanking_on(const struct device *dev) {
    int err;

    err = axs15231b_tx_cmd(dev, AXS_CMD_DISPOFF, NULL, 0);
    if (err) {
        return err;
    }
    err = axs15231b_tx_cmd(dev, AXS_CMD_SLPIN, NULL, 0);
    if (err) {
        return err;
    }

    return axs15231b_set_backlight(dev, false);
}

static int axs15231b_blanking_off(const struct device *dev) {
    int err;

    err = axs15231b_tx_cmd(dev, AXS_CMD_SLPOUT, NULL, 0);
    if (err) {
        return err;
    }
    k_msleep(AXS_SLPOUT_DELAY_MS);

    err = axs15231b_tx_cmd(dev, AXS_CMD_DISPON, NULL, 0);
    if (err) {
        return err;
    }
    k_msleep(AXS_DISPON_DELAY_MS);

    return axs15231b_set_backlight(dev, true);
}

static void axs15231b_get_capabilities(const struct device *dev,
                                       struct display_capabilities *caps) {
    ARG_UNUSED(dev);
    memset(caps, 0, sizeof(*caps));
    caps->x_resolution = AXS_WIDTH;
    caps->y_resolution = AXS_HEIGHT;
    caps->supported_pixel_formats = PIXEL_FORMAT_RGB_565;
    caps->current_pixel_format = PIXEL_FORMAT_RGB_565;
    caps->screen_info = 0;
}

static int axs15231b_set_pixel_format(const struct device *dev,
                                      const enum display_pixel_format pf) {
    ARG_UNUSED(dev);
    return (pf == PIXEL_FORMAT_RGB_565) ? 0 : -ENOTSUP;
}

static int axs15231b_set_orientation(const struct device *dev,
                                     const enum display_orientation orientation) {
    ARG_UNUSED(dev);
    /* Orientation is fixed at init (MADCTL 0x00). */
    return (orientation == DISPLAY_ORIENTATION_NORMAL) ? 0 : -ENOTSUP;
}

static int axs15231b_init(const struct device *dev) {
    const struct axs15231b_cfg *cfg = dev->config;
    struct axs15231b_data *data = dev->data;

    if (!spi_is_ready_dt(&cfg->spi)) {
        LOG_ERR("SPI bus not ready for AXS15231B");
        return -ENODEV;
    }

    /*
     * Derive the base SPI operation flags from the DT config: master mode,
     * 8-bit words, and the clock mode (CPOL/CPHA) from the devicetree. The
     * line mode (1-wire vs 4-wire) is set per transfer.
     */
    data->base_op = SPI_OP_MODE_MASTER | SPI_WORD_SET(8) |
                    (cfg->spi.config.operation & (SPI_MODE_CPOL | SPI_MODE_CPHA));

    if (cfg->reset.port != NULL) {
        if (!device_is_ready(cfg->reset.port)) {
            LOG_ERR("Reset GPIO port not ready");
            return -ENODEV;
        }
        gpio_pin_configure_dt(&cfg->reset, GPIO_OUTPUT_INACTIVE);
    }

#if AXS_HAS_BACKLIGHT
    if (!pwm_is_ready_dt(&cfg->backlight)) {
        LOG_ERR("Backlight PWM not ready");
        return -ENODEV;
    }
#endif

    return axs15231b_controller_init(dev);
}

static const struct display_driver_api axs15231b_api = {
    .write = axs15231b_write,
    .blanking_on = axs15231b_blanking_on,
    .blanking_off = axs15231b_blanking_off,
    .get_capabilities = axs15231b_get_capabilities,
    .set_pixel_format = axs15231b_set_pixel_format,
    .set_orientation = axs15231b_set_orientation,
};

static const struct axs15231b_cfg axs15231b_cfg = {
    .spi = SPI_DT_SPEC_GET(DT_INST(0, axs_axs15231b), SPI_OP_MODE_MASTER | SPI_WORD_SET(8)),
    .reset = GPIO_DT_SPEC_GET_OR(DT_INST(0, axs_axs15231b), reset_gpios, {0}),
#if AXS_HAS_BACKLIGHT
    .backlight = PWM_DT_SPEC_GET(DT_INST(0, axs_axs15231b)),
#endif
};

static struct axs15231b_data axs15231b_data;

DEVICE_DT_INST_DEFINE(0, axs15231b_init, NULL, &axs15231b_data, &axs15231b_cfg, POST_KERNEL,
                      CONFIG_DISPLAY_INIT_PRIORITY, &axs15231b_api);
