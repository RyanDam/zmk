/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * AXS15231B QSPI 320x480 panel driver (ESP32 MSPI controller). No RST or DC pin
 * is wired, so the panel is QSPI-only and reset with SWRESET.
 *
 * Wire protocol (datasheet, confirmed on the bench):
 *   - A frame opens with a 32-bit header on one data line:
 *     [opcode][0x00][cmd][0x00]; 0x02 = command + params (1-wire data),
 *     0x32 = color (pixels on all four lines).
 *   - Header and payload are ONE transaction under one CS assertion: the header
 *     rides the 16-bit command and address phases (1-wire in every io_mode),
 *     the payload is the data phase — see axs15231b_mspi_frame().
 *   - SPI mode 3, which on a GPIO-matrix clock pad needs the matrix inverter —
 *     see axs_mspi_clock_invert().
 *
 * The panel must be a child of an "espressif,esp32-mspi" node: the SPI API
 * cannot put a header and its pixels in one hardware transaction.
 */

#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/mspi.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/kernel.h>
#include <zephyr/linker/sections.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>
#include <soc/gpio_struct.h> /* gpio_dev_t, for the GPIO-matrix clock inverter */

#include <zmk/axs15231b_recovery.h>

#define DT_DRV_COMPAT axs_axs15231b

LOG_MODULE_REGISTER(axs15231b, CONFIG_DISPLAY_LOG_LEVEL);

#define AXS_WIDTH DT_INST_PROP(0, width)
#define AXS_HEIGHT DT_INST_PROP(0, height)

#define AXS_HAS_BACKLIGHT DT_NODE_HAS_PROP(DT_INST(0, axs_axs15231b), pwms)

/*
 * Orientation: `rotation` on the node is the angle the CONTENT turns, so a
 * portrait 320x480 panel shows a 480x320 UI. Software route (default): MADCTL
 * stays 0x00, LVGL renders landscape and axs15231b_write transposes the flush
 * into a PSRAM staging frame and streams one RAMWR. Hardware route
 * (CONFIG_AXS15231B_ROTATION_HW) uses MADCTL MV: zero per-frame cost, but the
 * row-pointer stream under MV is unproven and the MADCTL table below is only
 * the MIPI-DDI convention — bench-verify before that route ships. The transpose
 * mirrors lv_display_rotate_point(), which also maps touch, so pixels and taps
 * cannot drift: keep them in lockstep across an LVGL upgrade.
 */
#define AXS_ROTATION DT_PROP_OR(DT_INST(0, axs_axs15231b), rotation, 0)
#define AXS_ROTATION_HW IS_ENABLED(CONFIG_AXS15231B_ROTATION_HW)
#define AXS_ROTATION_SW (AXS_ROTATION != 0 && !AXS_ROTATION_HW)

BUILD_ASSERT(AXS_ROTATION == 0 || AXS_ROTATION == 90 || AXS_ROTATION == 180 ||
                     AXS_ROTATION == 270,
             "rotation: one of 0/90/180/270");

/* MADCTL bits per rotation (MX 0x80, MY 0x40, MV 0x20). */
#define AXS_MADCTL_ROT_0 0x00
#define AXS_MADCTL_ROT_90 0x60  /* MV | MX */
#define AXS_MADCTL_ROT_180 0xC0 /* MX | MY */
#define AXS_MADCTL_ROT_270 0xA0 /* MV | MY */

/* Logical (LVGL/flush) vs physical (what the glass streams); 90/270 swap. */
#define AXS_ROT_SWAPPED (AXS_ROTATION_HW && (AXS_ROTATION == 90 || AXS_ROTATION == 270))
#define AXS_LOGICAL_WIDTH (AXS_ROT_SWAPPED ? AXS_HEIGHT : AXS_WIDTH)
#define AXS_LOGICAL_HEIGHT (AXS_ROT_SWAPPED ? AXS_WIDTH : AXS_HEIGHT)

BUILD_ASSERT(DT_NODE_HAS_COMPAT(DT_PARENT(DT_INST(0, axs_axs15231b)), espressif_esp32_mspi),
             "this axs,axs15231b node must be a child of an espressif,esp32-mspi controller: "
             "header and pixels are one transaction here, and only the MSPI API can frame them");

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
#define AXS_CMD_RAMWRC 0x3C /* continue a write started by RAMWR, same window */
#define AXS_CMD_MADCTL 0x36
#define AXS_CMD_COLMOD 0x3A

/* Delays (ms) from the reference bsp / panel datasheet */
#define AXS_SWRESET_DELAY_MS 120
#define AXS_SLPOUT_DELAY_MS 120
#define AXS_DISPON_DELAY_MS 20

/* Pixel-phase line width, from the node's mspi-io-mode (binding enum order
 * matches enum mspi_io_mode so the index is the value). */
#define AXS_MSPI_PIXELS_IO_MODE                                                                          \
    ((enum mspi_io_mode)DT_ENUM_IDX_OR(DT_INST(0, axs_axs15231b), mspi_io_mode, MSPI_IO_MODE_SINGLE))

BUILD_ASSERT(AXS_MSPI_PIXELS_IO_MODE == MSPI_IO_MODE_SINGLE ||
                     AXS_MSPI_PIXELS_IO_MODE == MSPI_IO_MODE_QUAD_1_1_4,
             "mspi-io-mode: the header is always one line (command/address phases); the "
             "pixel phase is 1-1-1 or 1-1-4, nothing else");

struct axs15231b_cfg {
    const struct device *mspi_bus;
    /* The controller identifies a device by the *address* of this struct, so
     * it lives in the long-lived device config, not on a stack. Only dev_idx
     * matters: the CE line comes from the controller node's ce-gpios. */
    struct mspi_dev_id mspi_dev_id;
    struct mspi_dev_cfg mspi_dev_cfg;
    struct gpio_dt_spec reset;
#if AXS_HAS_BACKLIGHT
    struct pwm_dt_spec backlight;
#endif
};

struct axs15231b_data {
    /* Current line width. mspi_dev_config() takes the controller lock and
     * reprograms, so it runs only when the phase actually changes.
     * MSPI_IO_MODE_MAX starts the cache empty; a plain zero reads as SINGLE. */
    enum mspi_io_mode mspi_io_mode;

    bool initialized;
};

/*
 * The MSPI API has no per-transfer line width — it is device config
 * (MSPI_DEVICE_CONFIG_IO_MODE), read at the start of each transfer. Switching
 * the parameter phase (1-1-1) and the pixel phase (1-1-4) costs one call and
 * no register reprogramming.
 */
static int axs15231b_mspi_io_mode(const struct device *dev, enum mspi_io_mode io_mode) {
    const struct axs15231b_cfg *cfg = dev->config;
    struct axs15231b_data *data = dev->data;
    struct mspi_dev_cfg dev_cfg;

    if (data->mspi_io_mode == io_mode) {
        return 0;
    }

    dev_cfg = cfg->mspi_dev_cfg;
    dev_cfg.io_mode = io_mode;

    int err = mspi_dev_config(cfg->mspi_bus, &cfg->mspi_dev_id, MSPI_DEVICE_CONFIG_IO_MODE,
                              &dev_cfg);
    if (err) {
        LOG_ERR("MSPI I/O mode %d failed: %d", io_mode, err);
        return err;
    }

    data->mspi_io_mode = io_mode;

    return 0;
}

/*
 * One frame as ONE transaction: header [opcode][0x00][cmd][0x00] across the
 * command and address phases (one line each), payload as the data phase.
 * One CS assertion, no mid-frame mode switch.
 *
 * Splitting header and payload into two transfers is not safe: if CS wavers at
 * the seam the panel parses the payload's first bytes as a header, which paints
 * noise and decodes pixel bytes like 0x10 (SLEEP IN) as commands.
 */
static int axs15231b_mspi_frame(const struct device *dev, uint8_t opcode, uint8_t cmd,
                                const void *payload, size_t payload_len,
                                enum mspi_io_mode io_mode) {
    const struct axs15231b_cfg *cfg = dev->config;
    struct mspi_xfer_packet packet = {
        .dir = MSPI_TX,
        /* Command/address phases pack MSB-first: [opcode][0x00], [cmd][0x00]. */
        .cmd = (uint32_t)opcode << 8,
        .address = (uint32_t)cmd << 8,
        .num_bytes = (uint32_t)payload_len,
        .data_buf = (uint8_t *)payload,
    };
    struct mspi_xfer xfer = {
        /* Payloads over a FIFO's worth ride GDMA; command frames stay on PIO. */
        .xfer_mode = payload_len > 32U ? MSPI_DMA : MSPI_PIO,
        .cmd_length = 2,
        .addr_length = 2,
        .hold_ce = false,
        .packets = &packet,
        .num_packet = 1,
        /* The controller's ~100 ms default timeout is sized for flash reads; a
         * 300 kB RGB565 frame runs over 2 s at 1 MHz. Ask for twice the
         * transfer's own duration (clock divider may round down). */
        .timeout = (uint32_t)((2ULL * payload_len * 8ULL * 1000ULL) /
                              MAX(cfg->mspi_dev_cfg.freq, 1U)) +
                   50U,
    };
    int err = axs15231b_mspi_io_mode(dev, io_mode);

    if (err) {
        return err;
    }

    return mspi_transceive(cfg->mspi_bus, &cfg->mspi_dev_id, &xfer);
}

/* Send a command and optional parameters, header and parameters on one line. */
static int axs15231b_tx_cmd(const struct device *dev, uint8_t cmd, const void *params,
                            size_t params_len) {
    return axs15231b_mspi_frame(dev, AXS_OPCODE_WRITE_CMD, cmd, params, params_len,
                                MSPI_IO_MODE_SINGLE);
}

/* Stream a pixel frame: color header, then pixels on the node's wide lines. */
static int axs15231b_tx_color(const struct device *dev, uint8_t cmd, const void *pixels,
                              size_t pixels_len) {
    return axs15231b_mspi_frame(dev, AXS_OPCODE_WRITE_COLOR, cmd, pixels, pixels_len,
                                AXS_MSPI_PIXELS_IO_MODE);
}

#if AXS_HAS_BACKLIGHT
static int axs15231b_set_backlight(const struct device *dev, bool on) {
    const struct axs15231b_cfg *cfg = dev->config;

    /* One ns short of the period: at 0 %/100 % duty pwm_led_esp32.c stops the
     * counter and holds the pad at the idle level, and this backlight never
     * came up that way. The reference bsp likewise treats 100 % as 1023/1024. */
    uint32_t pulse = on ? (cfg->backlight.period - 1) : 0;
    int err = pwm_set_dt(&cfg->backlight, cfg->backlight.period, pulse);

    if (err) {
        LOG_ERR("Backlight PWM failed: %d", err);
    }

    return err;
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

/* Panel init sequence, verbatim from the JC3248W535 reference bsp
 * (esp_bsp.c lcd_init_cmds): panel-tuned, differs from the Espressif driver's
 * built-in default, so use it as-is. */
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
    {0x13, NULL, 0, 0},                   /* NORON: normal display mode on */
    {0x11, NULL, 0, AXS_SLPOUT_DELAY_MS}, /* SLPOUT */
    {0x2C, (const uint8_t[]){0x00, 0x00, 0x00, 0x00}, 4, 0},
};

/*
 * Run the reset-to-display sequence. Order taken from the reference bsp:
 * reset, SLPOUT, MADCTL, COLMOD, the 32-entry vendor table, SLPOUT,
 * DISPLAY_ON.
 */
static int axs15231b_send_init(const struct device *dev) {
    const struct axs15231b_cfg *cfg = dev->config;
    int err;

    if (cfg->reset.port != NULL) {
        /* Optional hardware reset (not wired on the JC3248W535 / cobantab1a). */
        gpio_pin_set_dt(&cfg->reset, 1);
        k_msleep(10);
        gpio_pin_set_dt(&cfg->reset, 0);
        k_msleep(10);
        gpio_pin_set_dt(&cfg->reset, 1);
        k_msleep(AXS_SWRESET_DELAY_MS);
    } else {
        err = axs15231b_tx_cmd(dev, AXS_CMD_SWRESET, NULL, 0);
        if (err) {
            LOG_ERR("SWRESET failed: %d", err);
            return err;
        }
        k_msleep(AXS_SWRESET_DELAY_MS);
    }

    /* Wake first: after power-on reset the panel is asleep, and the vendor
     * commands (boost, gate/source drivers, gamma) do not take effect until it
     * is out of sleep — otherwise the backlight lights a dead screen and the
     * touch block never answers on I2C. */
    err = axs15231b_tx_cmd(dev, AXS_CMD_SLPOUT, NULL, 0);
    if (err) {
        LOG_ERR("SLPOUT failed: %d", err);
        return err;
    }
    k_msleep(AXS_SLPOUT_DELAY_MS);

#if AXS_ROTATION_HW
    /* Hardware route: unproven on the bench, see the rotation note above. */
    uint8_t madctl = (AXS_ROTATION == 90)      ? AXS_MADCTL_ROT_90
                     : (AXS_ROTATION == 180)   ? AXS_MADCTL_ROT_180
                     : (AXS_ROTATION == 270)   ? AXS_MADCTL_ROT_270
                                               : AXS_MADCTL_ROT_0;
#else
    uint8_t madctl = 0x00; /* no swap, no mirror: rotation (if any) is software */
#endif
    err = axs15231b_tx_cmd(dev, AXS_CMD_MADCTL, &madctl, 1);
    if (err) {
        LOG_ERR("MADCTL failed: %d", err);
        return err;
    }

    uint8_t colmod = 0x55; /* 16-bit/pixel RGB565 */
    err = axs15231b_tx_cmd(dev, AXS_CMD_COLMOD, &colmod, 1);
    if (err) {
        LOG_ERR("COLMOD failed: %d", err);
        return err;
    }

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

    /* As the reference does on disp_on_off(false): exit sleep, then display on
     * (the table's trailing SLPOUT alone leaves the display off). */
    err = axs15231b_tx_cmd(dev, AXS_CMD_SLPOUT, NULL, 0);
    if (err) {
        LOG_ERR("SLPOUT failed: %d", err);
        return err;
    }
    k_msleep(AXS_SLPOUT_DELAY_MS);

    err = axs15231b_tx_cmd(dev, AXS_CMD_DISPON, NULL, 0);
    if (err) {
        LOG_ERR("DISPON failed: %d", err);
        return err;
    }
    k_msleep(AXS_DISPON_DELAY_MS);

    return 0;
}

#if AXS_ROTATION_SW
/*
 * Software rotation: the flush is one full logical frame (e.g. 480x320 on a
 * 320x480 panel at 90/270); transpose it into physical strips and stream those.
 * Direction mirrors lv_display_rotate_point() (also what maps touch):
 *   90:  P[y][x] = L[x][PH-1-y]
 *   270: P[y][x] = L[PW-1-x][y]
 *   180: P[y][x] = L[PH-1-y][PW-1-x]
 * Reads walk the PSRAM source row-wise, writes use a small internal-RAM
 * scratch, so no 300 KB staging buffer is needed.
 */
#define AXS_ROTATE_STRIP_ROWS 8

static uint16_t axs_rotate_scratch[AXS_ROTATE_STRIP_ROWS * AXS_WIDTH] __aligned(4);

/*
 * Full physical frame (300 KB) in PSRAM, on the same GDMA path as
 * `.lvgl_buf`: the transpose fills it and streams ONE RAMWR. A frame split
 * into many CS-asserted RAMWRC chunks is not proven on this panel (60-chunk
 * streaming painted scattered pixel runs).
 */
static uint16_t axs_rotate_frame[AXS_WIDTH * AXS_HEIGHT] Z_GENERIC_SECTION(.ext_ram.bss)
    __aligned(4);

static void axs_rotate_strip(const uint16_t *src, uint16_t pitch, uint16_t y0, uint16_t rows) {
    for (uint16_t x = 0; x < AXS_WIDTH; x++) {
        const uint16_t *scol = src + (uint32_t)x * pitch;

        for (uint16_t r = 0; r < rows; r++) {
#if AXS_ROTATION == 90
            axs_rotate_scratch[r * AXS_WIDTH + x] = scol[AXS_HEIGHT - 1 - (y0 + r)];
#elif AXS_ROTATION == 270
            axs_rotate_scratch[r * AXS_WIDTH + x] = src[(uint32_t)(AXS_WIDTH - 1 - x) * pitch +
                                                        y0 + r];
#elif AXS_ROTATION == 180
            axs_rotate_scratch[r * AXS_WIDTH + x] =
                src[(uint32_t)(AXS_HEIGHT - 1 - (y0 + r)) * pitch + (AXS_WIDTH - 1 - x)];
#endif
        }
    }
}

/* Full logical frame at the rotated shape — the only shape LVGL's full-refresh
 * mode emits and the only one the transpose serves. */
static bool axs_frame_is_rotated(const struct display_buffer_descriptor *desc) {
#if AXS_ROTATION == 90 || AXS_ROTATION == 270
    return desc->width == AXS_HEIGHT && desc->height == AXS_WIDTH;
#else
    return desc->width == AXS_WIDTH && desc->height == AXS_HEIGHT;
#endif
}

static int axs15231b_write_rotated(const struct device *dev,
                                   const struct display_buffer_descriptor *desc, const void *buf) {
    const uint16_t *src = buf;
    int err;

    /* CASET 0..AXS_WIDTH-1, as the unrotated path does for every full-width
     * flush: the window sets the row wrap, so off-by-one shears every row. */
    uint8_t caset[4] = {0x00, 0x00, (uint8_t)((AXS_WIDTH - 1) >> 8),
                        (uint8_t)((AXS_WIDTH - 1) & 0xFF)};

    err = axs15231b_tx_cmd(dev, AXS_CMD_CASET, caset, sizeof(caset));
    if (err) {
        return err;
    }

    /* Transpose strip by strip into the physical frame, then stream it once. */
    for (uint16_t y0 = 0; y0 < AXS_HEIGHT; y0 += AXS_ROTATE_STRIP_ROWS) {
        uint16_t rows = MIN(AXS_ROTATE_STRIP_ROWS, AXS_HEIGHT - y0);

        axs_rotate_strip(src, desc->pitch, y0, rows);
        memcpy(axs_rotate_frame + (uint32_t)y0 * AXS_WIDTH, axs_rotate_scratch,
               (size_t)rows * AXS_WIDTH * 2);
    }

    return axs15231b_tx_color(dev, AXS_CMD_RAMWR, axs_rotate_frame, sizeof(axs_rotate_frame));
}
#endif /* AXS_ROTATION_SW */

static int axs15231b_write(const struct device *dev, const uint16_t x, const uint16_t y,
                           const struct display_buffer_descriptor *desc, const void *buf) {
    struct axs15231b_data *data = dev->data;
    int err;

    if (!data->initialized) {
        return -ENODEV;
    }

#if AXS_ROTATION_SW
    if (x == 0 && y == 0 && axs_frame_is_rotated(desc)) {
        return axs15231b_write_rotated(dev, desc, buf);
    }
#endif

    uint16_t x_end = x + desc->width - 1;
    uint16_t y_end = y + desc->height - 1;

    if (x_end >= AXS_LOGICAL_WIDTH || y_end >= AXS_LOGICAL_HEIGHT) {
        LOG_ERR("Write out of bounds: x %u..%u, y %u..%u", x, x_end, y, y_end);
        return -EINVAL;
    }

    /* Program CASET and ONLY CASET: in QSPI the GRAM write is a row-pointer
     * stream — RASET is ignored, RAMWR restarts at row 0, RAMWRC continues from
     * the current pointer (the reference driver compiles RASET out of its QSPI
     * path). So flushes must be full-width from row 0, the shape
     * CONFIG_LV_Z_FULL_REFRESH produces; anything else writes the wrong rows. */
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

    if (y != 0) {
        LOG_WRN("AXS15231B: flush at y=%u cannot be placed (QSPI window is columns-only; "
                "run LVGL in full-refresh mode)", y);
    }

    /* RAMWR (0x2C) restarts the window, RAMWRC (0x3C) keeps the row pointer;
     * the row decides which, as in the reference driver. */
    const uint8_t ram_cmd = (y == 0) ? AXS_CMD_RAMWR : AXS_CMD_RAMWRC;

    /* desc->pitch (pixels) can exceed desc->width because LVGL aligns the
     * stride; RAMWR needs a contiguous stream, so copy when pitched. */
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
    caps->x_resolution = AXS_LOGICAL_WIDTH;
    caps->y_resolution = AXS_LOGICAL_HEIGHT;
    /* RGB_565X is byte-swapped 565: the panel consumes 16-bit pixels
     * big-endian like any MIPI DDI panel, and the flush streams VDB bytes
     * unchanged, so declaring the swapped format (LV maps it to
     * LV_COLOR_FORMAT_RGB565_SWAPPED) makes LVGL emit MSB-first for free.
     * Plain RGB_565 looks fine where 0x0000/0xFFFF dominate and turns every
     * anti-aliased grey into coloured speckle. */
    caps->supported_pixel_formats = PIXEL_FORMAT_RGB_565X;
    caps->current_pixel_format = PIXEL_FORMAT_RGB_565X;
    caps->screen_info = 0;
#if AXS_ROT_SWAPPED || (AXS_ROTATION_HW && AXS_ROTATION == 180)
    /* Touch glue maps raw points into these caps, so the hardware route must
     * not also rotate inside LVGL. */
    caps->current_orientation = (AXS_ROTATION == 90)    ? DISPLAY_ORIENTATION_ROTATED_90
                                : (AXS_ROTATION == 180) ? DISPLAY_ORIENTATION_ROTATED_180
                                                        : DISPLAY_ORIENTATION_ROTATED_270;
#endif
}

static int axs15231b_set_pixel_format(const struct device *dev,
                                      const enum display_pixel_format pf) {
    ARG_UNUSED(dev);
    return (pf == PIXEL_FORMAT_RGB_565X) ? 0 : -ENOTSUP;
}

static int axs15231b_set_orientation(const struct device *dev,
                                     const enum display_orientation orientation) {
    ARG_UNUSED(dev);
    /* Orientation is fixed at init from the node's `rotation`; the pointer glue
     * caches caps at its own init, so a runtime flip cannot work end-to-end. */
#if !AXS_ROTATION_HW
    /* Software route: the PANEL stays normal; only LVGL/flush rotate. */
    return (orientation == DISPLAY_ORIENTATION_NORMAL) ? 0 : -ENOTSUP;
#else
    const enum display_orientation want =
        (AXS_ROTATION == 90)    ? DISPLAY_ORIENTATION_ROTATED_90
        : (AXS_ROTATION == 180) ? DISPLAY_ORIENTATION_ROTATED_180
        : (AXS_ROTATION == 270) ? DISPLAY_ORIENTATION_ROTATED_270
                                : DISPLAY_ORIENTATION_NORMAL;

    return (orientation == want) ? 0 : -ENOTSUP;
#endif
}

/*
 * Mode 3 on this pad. mspi_dev_config() programs the controller's mode
 * register, but a clock driven through the GPIO matrix arrives one half-clock
 * out of step; IDF's spi_master reaches mode 3 by connecting the clock through
 * the matrix WITH THE INVERTER ENABLED, and that is the waveform this panel
 * accepts (measured: only mode 0 direct and mode 3 inverted painted). The
 * shield's pinctrl routes FSPICLK_OUT here un-inverted, so set the matrix
 * inverter bit — and keep the pad number equal to the overlay's clock pad.
 */
#define AXS_MSPI_SCLK_PAD 47

static void axs_mspi_clock_invert(const struct axs15231b_cfg *cfg) {
    if (cfg->mspi_dev_cfg.cpp != MSPI_CPP_MODE_3) {
        return;
    }

    gpio_dev_t *iomux = (gpio_dev_t *)DT_REG_ADDR(DT_NODELABEL(gpio0));

    iomux->func_out_sel_cfg[AXS_MSPI_SCLK_PAD].inv_sel = 1;
}

static int axs15231b_init(const struct device *dev) {
    const struct axs15231b_cfg *cfg = dev->config;
    struct axs15231b_data *data = dev->data;
    int err;

    if (!device_is_ready(cfg->mspi_bus)) {
        LOG_ERR("MSPI bus not ready for AXS15231B");
        return -ENODEV;
    }

    /* Ask the controller for its caps before claiming them: documented order,
     * it releases a lock a previous caller left held, and -EINVAL means the
     * node names a channel the controller does not have. */
    err = mspi_get_channel_status(cfg->mspi_bus, 0);
    if (err) {
        LOG_ERR("MSPI channel 0 not usable for AXS15231B: %d", err);
        return err;
    }

    data->mspi_io_mode = MSPI_IO_MODE_MAX;

    /* Clock rate, clock mode and CE in one call: mspi_esp32.c came up at the
     * controller node's own clock-frequency, so the panel's rate must be asked. */
    err = mspi_dev_config(cfg->mspi_bus, &cfg->mspi_dev_id,
                          MSPI_DEVICE_CONFIG_FREQUENCY | MSPI_DEVICE_CONFIG_CPP |
                              MSPI_DEVICE_CONFIG_DATA_RATE | MSPI_DEVICE_CONFIG_CE_NUM |
                              MSPI_DEVICE_CONFIG_CE_POL | MSPI_DEVICE_CONFIG_ENDIAN,
                          &cfg->mspi_dev_cfg);
    if (err) {
        LOG_ERR("MSPI device configuration failed: %d", err);
        return err;
    }

    /* Mode 3 only works on this pad with the GPIO-matrix inverter on. */
    axs_mspi_clock_invert(cfg);

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

    err = axs15231b_send_init(dev);
    if (err) {
        LOG_ERR("Panel init failed: %d", err);
        return err;
    }

    data->initialized = true;

    LOG_INF("AXS15231B initialized (%ux%u, rotation %u, %s route)", AXS_WIDTH, AXS_HEIGHT,
            (unsigned int)AXS_ROTATION,
            (AXS_ROTATION == 0) ? "upright" : (AXS_ROTATION_HW ? "hardware" : "software"));

    return 0;
}

int zmk_axs15231b_recover(const struct device *dev, bool hard) {
    int err;

    if (dev != DEVICE_DT_INST_GET(0)) {
        return -ENODEV;
    }

    LOG_WRN("recovering panel (%s reset)", hard ? "hard" : "soft");

    if (hard) {
        /* Sleep-in first: a deeper edge for the touch engine to exit on. */
        (void)axs15231b_tx_cmd(dev, AXS_CMD_SLPIN, NULL, 0);
        k_msleep(AXS_SLPOUT_DELAY_MS);
    }

    /* send_init resets, wakes the panel and replays the vendor table + DISPON. */
    err = axs15231b_send_init(dev);
    if (err) {
        LOG_ERR("panel recovery failed: %d", err);
        return err;
    }

    LOG_INF("panel recovered; GRAM is black until the UI repaints");
    return 0;
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
    .mspi_bus = DEVICE_DT_GET(DT_PARENT(DT_INST(0, axs_axs15231b))),
    .mspi_dev_id = {
        .dev_idx = DT_REG_ADDR(DT_INST(0, axs_axs15231b)),
    },
    .mspi_dev_cfg = {
        /* The child's reg is also its CE index on this controller. */
        .ce_num = DT_REG_ADDR(DT_INST(0, axs_axs15231b)),
        .freq = DT_INST_PROP(0, mspi_max_frequency),
        .io_mode = AXS_MSPI_PIXELS_IO_MODE,
        .data_rate = MSPI_DATA_RATE_SINGLE,
        .cpp = (enum mspi_cpp_mode)DT_ENUM_IDX_OR(DT_INST(0, axs_axs15231b), mspi_cpp_mode,
                                                   MSPI_CPP_MODE_3),
        /* Bytes always leave MSB first on this controller. */
        .endian = MSPI_XFER_BIG_ENDIAN,
        .ce_polarity = MSPI_CE_ACTIVE_LOW,
    },
    .reset = GPIO_DT_SPEC_GET_OR(DT_INST(0, axs_axs15231b), reset_gpios, {0}),
#if AXS_HAS_BACKLIGHT
    .backlight = PWM_DT_SPEC_GET(DT_INST(0, axs_axs15231b)),
#endif
};

static struct axs15231b_data axs15231b_data;

DEVICE_DT_INST_DEFINE(0, axs15231b_init, NULL, &axs15231b_data, &axs15231b_cfg, POST_KERNEL,
                      CONFIG_DISPLAY_INIT_PRIORITY, &axs15231b_api);
