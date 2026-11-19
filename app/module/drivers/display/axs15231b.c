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
 * QSPI wire protocol (from Espressif's esp_lcd_panel_io_spi.c, the layer the
 * JC3248W535 reference bsp drives, and the AXS15231B datasheet framing):
 *   - Every transfer opens with a 32-bit header on D0:
 *       [opcode][0x00][cmd][0x00]
 *     where opcode is 0x02 (write command) or 0x32 (write color). The opcode
 *     also announces the width of the data phase that follows it: 0x02 means
 *     parameters on D0 only, 0x32 means pixels on all four lines.
 *   - Command phase: header + parameters, 1-wire, one CS assertion.
 *   - Color phase: header in 1-wire, then pixels in 4-wire (QIO), under the
 *     same CS assertion. The reference keeps CS down there with
 *     SPI_TRANS_CS_KEEP_ACTIVE; here the driver holds CS itself, because
 *     Zephyr's ESP32 SPI driver ignores SPI_HOLD_ON_CS entirely.
 *
 * Getting the pixel data on four lines is what the CS handling and the two
 * spi_config objects below are for. Both are needed: the ESP32 cannot select a
 * quad data phase at all on the Zephyr revision this workspace pins (upstream
 * commit 24cc93286, carried in zmk/esp-port/patches/), and without a held CS
 * the panel never enters the memory write the header asked for.
 */

#define DT_DRV_COMPAT axs_axs15231b

#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/drivers/mspi.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>


LOG_MODULE_REGISTER(axs15231b, CONFIG_DISPLAY_LOG_LEVEL);

#define AXS_WIDTH DT_INST_PROP(0, width)
#define AXS_HEIGHT DT_INST_PROP(0, height)

#define AXS_HAS_BACKLIGHT DT_NODE_HAS_PROP(DT_INST(0, axs_axs15231b), pwms)

/*
 * Which controller this panel hangs off: an SPI bus or an MSPI bus. There is no
 * Kconfig for it - the devicetree says where the chip is wired, so that is what
 * gets read. Everything below that is specific to one of the two paths is
 * written against these two symbols.
 */
#define AXS_ON_MSPI DT_NODE_HAS_COMPAT(DT_PARENT(DT_INST(0, axs_axs15231b)), espressif_esp32_mspi)

/* The four bring-up instruments that reach into the SPI core, or drive the CS
 * pad from this driver, describe the SPI path and have no MSPI counterpart. */
#define AXS_SPI_PATH (!AXS_ON_MSPI)

#if AXS_ON_MSPI && !IS_ENABLED(CONFIG_MSPI)
#error "this axs,axs15231b node is a child of an MSPI controller: enable CONFIG_MSPI"
#endif

#if AXS_SPI_PATH && !IS_ENABLED(CONFIG_SPI_EXTENDED_MODES)
#error "the pixel phase needs SPI_LINES_QUAD: enable CONFIG_SPI_EXTENDED_MODES"
#endif

/* QSPI opcodes (top byte of the 32-bit command word) */
#define AXS_OPCODE_WRITE_CMD 0x02
#define AXS_OPCODE_WRITE_COLOR 0x32

/*
 * Slave number used on both spi_configs. The ESP32 peripheral maps
 * cs_pin_id 0-2 to its hardware chip selects and ignores anything else, and
 * this driver drives CS itself, so it claims a number outside that range to
 * keep the peripheral's CS lines unused.
 */
#if AXS_SPI_PATH
#define AXS_NO_HW_CS_SLAVE 3
#endif

#if AXS_ON_MSPI
/*
 * The frame header goes out as an ordinary 1-wire data phase, ahead of the data
 * phase that carries its payload. That is how the reference driver frames it
 * (esp_lcd_panel_io_spi's tx_param/tx_color set `length = lcd_cmd_bits` with
 * `tx_buffer = &cmd`, and only the pixels get SPI_TRANS_MODE_QIO), and how the
 * SPI path in this same file frames it - which is the framing this panel has
 * visibly responded to.
 *
 * It is deliberately not the address phase. The ESP32 address phase is wider
 * than the 16-bit command field (MSPI_ESP32_MAX_CMD_LEN in mspi_esp32.c), so it
 * looked like the tidy way to carry [opcode][0x00][cmd][0x00] in a single
 * transaction with the controller holding CS. The controller does clock it out,
 * and every number readable in software says the transfer succeeded; the panel
 * has never reacted to anything sent that way.
 */
#define AXS_MSPI_HEADER_BYTES 4

/* Where the pixel phase goes, from the node's mspi-io-mode. The binding keeps
 * its enum list in the order of enum mspi_io_mode so the index can be used as
 * the value. */
#define AXS_MSPI_PIXELS_IO_MODE                                                                          \
    ((enum mspi_io_mode)DT_ENUM_IDX_OR(DT_INST(0, axs_axs15231b), mspi_io_mode, MSPI_IO_MODE_SINGLE))

BUILD_ASSERT(AXS_MSPI_PIXELS_IO_MODE == MSPI_IO_MODE_SINGLE ||
                     AXS_MSPI_PIXELS_IO_MODE == MSPI_IO_MODE_QUAD_1_1_4,
             "mspi-io-mode: the panel decodes its 32-bit header on one line, so the pixel "
             "phase can be 1-1-1 (SINGLE) or 1-1-4 (QUAD_1_1_4), nothing else");
#endif

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

struct axs15231b_cfg {
#if AXS_ON_MSPI
    const struct device *mspi_bus;
    /*
     * The controller identifies the device by the *address of this struct*, so
     * it lives in the (static, long-lived) device configuration and not on a
     * stack: mspi_esp32_transceive() refuses a transfer whose dev_id pointer is
     * not the one mspi_esp32_dev_config() was last called with. Only dev_idx is
     * meaningful here - the ESP32 driver picks the CE line from the controller
     * node's ce-gpios via the configured ce_num, never from this struct.
     */
    struct mspi_dev_id mspi_dev_id;
    struct mspi_dev_cfg mspi_dev_cfg;
#else
    struct spi_dt_spec spi;
    struct gpio_dt_spec cs;
#endif
    struct gpio_dt_spec reset;
#if AXS_HAS_BACKLIGHT
    struct pwm_dt_spec backlight;
#endif
};

struct axs15231b_data {
#if AXS_SPI_PATH
    /*
     * The two line widths this panel needs, built once in axs15231b_init() and
     * reused from then on. The ESP32 SPI driver decides "same configuration?" by
     * comparing the spi_config *pointer*, so these are long-lived objects with
     * stable addresses rather than locals at each transfer.
     */
    struct spi_config spi_cmd;    /* header (+ parameters), 1-wire */
    struct spi_config spi_pixels; /* pixel data, 4-wire (QIO) */
#else
    /*
     * The line width the controller is currently set to. Switching between the
     * 1-wire and 4-wire phases is a dev_config() call, and mspi_dev_config() is
     * not free (it takes the controller's lock and reprograms it), so it is
     * called only when the phase actually changes. MSPI_IO_MODE_MAX is not a
     * mode any device can ask for, so it starts the cache empty (set in
     * axs15231b_init(); a plain zero would look like MSPI_IO_MODE_SINGLE).
     */
    enum mspi_io_mode mspi_io_mode;
#endif

    bool initialized;
#if IS_ENABLED(CONFIG_AXS15231B_COLOR_TEST) || IS_ENABLED(CONFIG_AXS15231B_FILL_TEST)
    /*
     * Set once a bring-up paint has finished: further writes are dropped, so the
     * pattern stays on the panel to be looked at instead of being overdrawn by
     * the status screen a few milliseconds later.
     */
    bool paint_hold;
#endif
};

#if AXS_SPI_PATH
/*
 * Chip select is driven by this driver, not handed to the SPI core.
 *
 * The panel wants the 32-bit header on one data line and the pixels belonging
 * to it on four, under a single CS assertion: esp_lcd_panel_io_spi.c sends the
 * header with spi_device_polling_transmit() plus SPI_TRANS_CS_KEEP_ACTIVE and
 * puts SPI_TRANS_MODE_QIO on the color chunks only ("use 4 lines only for
 * transmitting data"). Zephyr's ESP32 driver never reads SPI_HOLD_ON_CS, so
 * leaving CS to it would raise the line between a header and its pixels.
 *
 * With no cs-gpios on the bus node the driver does not touch CS at all
 * (spi_context.h defines DT_SPI_CTX_HAS_NO_CS_GPIOS for it), which incidentally
 * also compiles out that driver's mode-3 "dummy transaction needed to sync CLK
 * and software controlled CS" workaround - the phantom transfer that used to
 * eat the first FIFO load of every new configuration.
 */
static void axs15231b_cs_select(const struct device *dev) {
    const struct axs15231b_cfg *cfg = dev->config;

    (void)gpio_pin_set_dt(&cfg->cs, 1);
}

static void axs15231b_cs_release(const struct device *dev) {
    const struct axs15231b_cfg *cfg = dev->config;

    (void)gpio_pin_set_dt(&cfg->cs, 0);
}

/*
 * Spend the first clock edge of a frame where the panel cannot see it.
 *
 * Zephyr's ESP32 driver does this for its own callers and calls it a workaround:
 * "dummy transaction needed to sync CLK and software controlled CS when SPI is
 * in mode 3" (spi_esp32_configure). It fires a transfer while the SPI core has
 * not asserted CS yet, so the first edge after a reconfigure - which does not
 * yet come out at the configured idle level - is spent on nothing.
 *
 * That workaround is compiled out for us (it is guarded on ctx->num_cs_gpios,
 * and here CS belongs to this driver, not to the SPI core), and its absence is
 * not neutral. In mode 3 SCLK idles high; after a reconfigure the pad does not
 * necessarily sit there until a transfer has run. So CS falls while SCLK is at
 * the wrong level, and the first edge the panel sees after CS falls is a latch
 * edge rather than a launch edge: the frame is one bit out of step, from
 * then on, for its whole length. A QSPI slave counts bits from the falling edge
 * of CS and resynchronises on the rising one, so the result is a stream that is
 * byte-for-byte correct and completely unintelligible - which is exactly what a
 * panel that acknowledges nothing looks like, and no register readback can show
 * it, because every register is set correctly.
 *
 * Priming with CS raised gives the peripheral its first edge for free, so the
 * frame that follows starts cleanly. Upstream does the same thing, just by
 * eating the first chunk of the real transfer instead of a byte of its own.
 */
static int axs15231b_prime(const struct device *dev, const struct spi_config *spi_cfg) {
    const struct axs15231b_cfg *cfg = dev->config;
    static uint8_t prime_buf;
    struct spi_buf buf = {.buf = &prime_buf, .len = sizeof(prime_buf)};
    struct spi_buf_set set = {.buffers = &buf, .count = 1};

    return spi_transceive(cfg->spi.bus, spi_cfg, &set, NULL);
}
#endif /* AXS_SPI_PATH */

#if AXS_ON_MSPI
/*
 * Put the controller in the line mode the next frame needs.
 *
 * The MSPI API carries no per-transfer line width: how many lines the command,
 * address and data phases use is device configuration, set with
 * mspi_dev_config(MSPI_DEVICE_CONFIG_IO_MODE). mspi_esp32.c picks the phase
 * widths up from it at the start of every transfer, and changing it does not
 * dirty the device-level registers, so switching between the parameter phase
 * (1-1-1) and the pixel phase (1-1-4) costs one call and no reprogramming -
 * still worth skipping when the mode is already right.
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
 * One TX transfer on this bus: @a len bytes from @a buf, on the data lines
 * @a io_mode selects, with CE released afterwards unless @a hold_ce says another
 * transfer of the same frame follows. The line width is device configuration
 * rather than a property of a transfer, so anything that changes width has to
 * change mode with it - done here, where the two belong together.
 */
static int axs15231b_mspi_write(const struct device *dev, const void *buf, size_t len,
                                enum mspi_io_mode io_mode, enum mspi_xfer_mode mode,
                                bool hold_ce) {
    const struct axs15231b_cfg *cfg = dev->config;
    struct mspi_xfer_packet packet = {
        .dir = MSPI_TX,
        .num_bytes = (uint32_t)len,
        .data_buf = (uint8_t *)buf,
    };
    struct mspi_xfer xfer = {
        .xfer_mode = mode,
        .hold_ce = hold_ce,
        .packets = &packet,
        .num_packet = 1,
        /*
         * The controller's default timeout (its transfer-timeout, 100 ms unless
         * the node says otherwise) is sized for flash reads. A panel frame is
         * much longer at a bring-up clock: a whole 320x480 RGB565 frame is
         * 300 kB, over 2 s at 1 MHz. So ask for twice the transfer's own
         * duration, in case the clock divider rounded down.
         */
        .timeout = (uint32_t)((2ULL * len * 8ULL * 1000ULL) / MAX(cfg->mspi_dev_cfg.freq, 1U)) +
                   50U,
    };
    int err = axs15231b_mspi_io_mode(dev, io_mode);

    if (err) {
        return err;
    }

    return mspi_transceive(cfg->mspi_bus, &cfg->mspi_dev_id, &xfer);
}

/*
 * One frame, framed the way the reference driver frames it: the 32-bit header
 * [opcode][0x00][cmd][0x00] as a 1-wire data phase, then @a payload on @a
 * io_mode's data lines, with CE held low across both so the panel reads them as
 * one frame.
 *
 * Two transfers, because the line width changes between the header and its
 * pixels and that change has to happen between them; hold_ce is what keeps the
 * panel inside the frame while it does. A header with no payload is just the
 * header with CE released - the parameter-less commands.
 */
static int axs15231b_mspi_frame(const struct device *dev, uint8_t opcode, uint8_t cmd,
                                const void *payload, size_t payload_len,
                                enum mspi_io_mode io_mode) {
    /* Sent in memory order, so the opcode is the first byte on the wire. */
    uint8_t header[AXS_MSPI_HEADER_BYTES] __aligned(4) = {opcode, 0x00, cmd, 0x00};
    const bool has_payload = payload_len > 0;
    int err;

    err = axs15231b_mspi_write(dev, header, sizeof(header), MSPI_IO_MODE_SINGLE, MSPI_PIO,
                               has_payload);
    if (err) {
        return err;
    }

    if (!has_payload) {
        return 0;
    }

    return axs15231b_mspi_write(dev, payload, payload_len, io_mode, MSPI_DMA, false);
}
#endif /* AXS_ON_MSPI */

/*
 * Send a 32-bit command opcode followed by optional parameters, all in 1-wire
 * mode, in a single CS transaction.
 */
static int axs15231b_tx_cmd(const struct device *dev, uint8_t cmd, const void *params,
                            size_t params_len) {
#if AXS_ON_MSPI
    return axs15231b_mspi_frame(dev, AXS_OPCODE_WRITE_CMD, cmd, params, params_len,
                                 MSPI_IO_MODE_SINGLE);
#else
    const struct axs15231b_cfg *cfg = dev->config;
    struct axs15231b_data *data = dev->data;

    /*
     * 32-bit QSPI command word, on the wire MSB-first:
     *   [opcode][0x00][cmd][0x00]
     * The opcode (0x02) is the top byte and the LCD command is the third byte.
     * (Matches the reference bsp, which builds the word as
     * (opcode << 24) | (cmd << 8) and byte-reverses it for the big-endian LCD.)
     */
    uint8_t header[4] __aligned(4) = {
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
    int err;

    axs15231b_cs_select(dev);
    err = spi_transceive(cfg->spi.bus, &data->spi_cmd, &tx, NULL);
    axs15231b_cs_release(dev);

    return err;
#endif
}

#if AXS_SPI_PATH
/*
 * One CS assertion, two transfers: the 32-bit header under @a hdr_cfg, then the
 * payload under @a px_cfg. Opcode 0x02 promises a payload on D0 only, 0x32 one
 * on all four lines, so the opcode and the line mode of @a px_cfg have to agree.
 *
 * Both configs must be long-lived (the SPI driver compares pointers) and every
 * buffer has to stay valid until the second transfer is done, which is why the
 * caller passes one contiguous payload.
 */
static int axs15231b_tx_frame(const struct device *dev, const struct spi_config *hdr_cfg,
                              const struct spi_config *px_cfg, uint8_t opcode, uint8_t cmd,
                              const void *payload, size_t payload_len) {
    const struct axs15231b_cfg *cfg = dev->config;

    /* Header, 32-bit layout: [opcode][0x00][cmd][0x00]. */
    uint8_t header[4] __aligned(4) = {
        opcode,
        0x00,
        cmd,
        0x00,
    };
    struct spi_buf header_buf = {.buf = header, .len = sizeof(header)};
    struct spi_buf_set header_set = {.buffers = &header_buf, .count = 1};

    struct spi_buf payload_buf = {.buf = (void *)payload, .len = payload_len};
    struct spi_buf_set payload_set = {.buffers = &payload_buf, .count = 1};

    int err;

    axs15231b_cs_select(dev);

    err = spi_transceive(cfg->spi.bus, hdr_cfg, &header_set, NULL);
    if (!err) {
        err = spi_transceive(cfg->spi.bus, px_cfg, &payload_set, NULL);
    }

    axs15231b_cs_release(dev);

    return err;
}
#endif /* AXS_SPI_PATH */

/*
 * Stream a pixel frame the way the reference driver does: the 32-bit color
 * header in 1-wire mode, then the pixels on the wide data phase, under one CS
 * assertion. Opcode 0x32 is what tells the panel the pixels come on four
 * lines; opcode 0x02 with @a wide false keeps the whole frame on D0, so the
 * transfer is byte-for-byte what the wide pass sends apart from the width of
 * the data phase.
 *
 * @a wide is a parameter rather than only CONFIG_AXS15231B_PIXELS_1WIRE
 * because the fill test runs both passes back to back in one boot: together
 * they say whether anything at all reaches GRAM, and whether the fault is
 * specific to the wide phase. On the MSPI bus a pass is one I/O-mode change; on
 * SPI it is a different spi_config. Writes from LVGL take the mode from the
 * Kconfig, in axs15231b_write().
 */
static int axs15231b_tx_color_wide(const struct device *dev, uint8_t cmd, const void *pixels,
                                    size_t pixels_len, bool wide) {
#if AXS_ON_MSPI
    if (!wide) {
        return axs15231b_mspi_frame(dev, AXS_OPCODE_WRITE_CMD, cmd, pixels, pixels_len,
                                     MSPI_IO_MODE_SINGLE);
    }

    return axs15231b_mspi_frame(dev, AXS_OPCODE_WRITE_COLOR, cmd, pixels, pixels_len,
                                 AXS_MSPI_PIXELS_IO_MODE);
#else
    struct axs15231b_data *data = dev->data;

    if (!wide) {
        return axs15231b_tx_frame(dev, &data->spi_cmd, &data->spi_cmd, AXS_OPCODE_WRITE_CMD, cmd,
                                  pixels, pixels_len);
    }

    return axs15231b_tx_frame(dev, &data->spi_cmd, &data->spi_pixels, AXS_OPCODE_WRITE_COLOR, cmd,
                              pixels, pixels_len);
#endif
}

#if AXS_HAS_BACKLIGHT
static int axs15231b_set_backlight(const struct device *dev, bool on) {
    const struct axs15231b_cfg *cfg = dev->config;

    /*
     * One nanosecond short of the period rather than exactly it. A pulse equal
     * to the period makes Zephyr's LEDC driver stop the PWM counter and hold the
     * pad through the channel's idle level instead (pwm_led_esp32.c: "For duty
     * 0% and 100% stop PWM, set output level and return") - a path this panel's
     * backlight never came up on. The reference bsp stays one step below full
     * scale for the same reason: "LEDC resolution set to 10bits, thus: 100% =
     * 1023" (esp_bsp.c). Visually identical, and it keeps the counter running.
     */
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
    {0x13, NULL, 0, 0},                   /* NORON: normal display mode on */
    {0x11, NULL, 0, AXS_SLPOUT_DELAY_MS}, /* SLPOUT */
    {0x2C, (const uint8_t[]){0x00, 0x00, 0x00, 0x00}, 4, 0},
};

#if IS_ENABLED(CONFIG_AXS15231B_COLOR_TEST)
static int axs15231b_color_test(const struct device *dev);
#endif

#if IS_ENABLED(CONFIG_AXS15231B_FILL_TEST)
static int axs15231b_fill_test(const struct device *dev);
#endif

#if IS_ENABLED(CONFIG_AXS15231B_COMMAND_PROBE)
static void axs_cmd_probe_run(const struct device *dev);
#endif

#if IS_ENABLED(CONFIG_AXS15231B_FILL_TEST) || IS_ENABLED(CONFIG_AXS15231B_COMMAND_PROBE)
static void axs_bringup_start(const struct device *dev);
#endif

#if AXS_SPI_PATH && IS_ENABLED(CONFIG_AXS15231B_QSPI_TOUCH_PROBE)
static void axs_probe_start(const struct device *dev);
#endif


#if AXS_SPI_PATH && IS_ENABLED(CONFIG_AXS15231B_WIRE_PROBE)
/*
 * Wire probe (bring-up aid, ESP32-S3).
 *
 * Every layer above the wire has been compared against the reference driver, and
 * two things still cannot be told apart from software alone:
 *
 *   - whether the controller really puts the pixel phase on four data lines
 *     (the SPI2 line-mode bits are written by the ESP32 driver and never read
 *     back), and
 *   - whether the GPIO matrix really routes those signals to the four pads
 *     (a pad that was never connected to the matrix sits at whatever level its
 *     GPIO driver left it at, which is invisible to the SPI driver).
 *
 * Both are register state this firmware can read back, so do that. The transfer
 * timing is logged too, because bits per second alone cannot separate "four
 * lines at 1 MHz" from "one line at 4 MHz" - the register bits say how many
 * lines moved and the clock register says what the divider was asked for, so
 * together with the timing they pin down what is actually on the wire.
 */
#include <soc/gpio_sig_map.h>  /* SIG_GPIO_OUT_IDX */
#include <soc/gpio_struct.h>   /* gpio_dev_t */
#include <soc/spi_struct.h>    /* spi_dev_t */
#include <zephyr/dt-bindings/pinctrl/esp32s3-gpio-sigmap.h>

struct axs_wire_pad {
	uint8_t pad;
	uint16_t sig; /* the matrix output signal this pad must carry */
	const char *name;
};

/*
 * D2 is SPIWP and D3 is SPIHD, in that order: the same assignment
 * esp_spi_common.c makes for a quad bus (data0->spid, data1->spiq,
 * data2->spiwp, data3->spihd). CS is a plain GPIO output, which the matrix
 * represents by SIG_GPIO_OUT_IDX.
 */
static const struct axs_wire_pad axs_wire_pads[] = {
    {47, ESP_FSPICLK_OUT, "SCLK"}, {21, ESP_FSPID_OUT, "D0"},  {48, ESP_FSPIQ_OUT, "D1"},
    {40, ESP_FSPIWP_OUT, "D2"},    {39, ESP_FSPIHD_OUT, "D3"}, {45, SIG_GPIO_OUT_IDX, "CS"},
};

/* A whole number of FIFO loads; big enough that per-transfer overhead is a
 * rounding error in the timing. */
#define AXS_WIRE_BYTES 4096
static uint8_t axs_wire_pixels[AXS_WIRE_BYTES] __aligned(4);

static void axs15231b_wire_regs(const char *tag) {
    const spi_dev_t *spi = (const spi_dev_t *)DT_REG_ADDR(DT_NODELABEL(spi2));

    LOG_INF("wire %s: user=%08x ctrl=%08x clock=%08x misc=%08x", tag, spi->user.val,
            spi->ctrl.val, spi->clock.val, spi->misc.val);
    LOG_INF("wire %s: doutdin=%u fwrite_quad=%u fwrite_dual=%u fread_quad=%u ck_idle_edge=%u", tag,
            spi->user.doutdin, spi->user.fwrite_quad, spi->user.fwrite_dual, spi->ctrl.fread_quad,
            spi->misc.ck_idle_edge);
}

/*
 * Paint AXS_WIRE_BYTES with one frame shape and report what it cost. The clock
 * the transfer implies under each line-count hypothesis is printed because the
 * hypothesis that matches the frequency the device tree asked for is the one
 * that was really on the wire.
 */
static void axs15231b_wire_measure(const struct device *dev, const struct spi_config *hdr_cfg,
                                   const struct spi_config *px_cfg, uint8_t opcode,
                                   const char *tag) {
    struct axs15231b_data *data = dev->data;
    uint32_t t0 = k_cycle_get_32();
    int err = axs15231b_tx_frame(dev, hdr_cfg, px_cfg, opcode, AXS_CMD_RAMWR, axs_wire_pixels,
                                  sizeof(axs_wire_pixels));
    uint32_t us = MAX(k_cyc_to_us_floor32(k_cycle_get_32() - t0), 1U);
    const uint32_t bits = AXS_WIRE_BYTES * 8;

    LOG_INF("wire %s: %u B in %u us -> %u kbit/s", tag, (uint32_t)sizeof(axs_wire_pixels), us,
            (uint32_t)((bits / us) / 1000U));
    LOG_INF("wire %s: that is SCLK %u Hz on four lines, %u Hz on one (asked for %u Hz)", tag,
            (uint32_t)(bits / us / 4U), (uint32_t)(bits / us), data->spi_pixels.frequency);
    axs15231b_wire_regs(tag);

    if (err) {
        LOG_ERR("wire %s: transfer failed: %d", tag, err);
    }
}

static void axs15231b_wire_probe(const struct device *dev) {
    struct axs15231b_data *data = dev->data;
    const gpio_dev_t *gpio = (const gpio_dev_t *)DT_REG_ADDR(DT_NODELABEL(gpio0));

    LOG_INF("wire: probe start (spi2, %u Hz requested)", data->spi_cmd.frequency);

    for (size_t i = 0; i < ARRAY_SIZE(axs_wire_pads); i++) {
        const struct axs_wire_pad *p = &axs_wire_pads[i];
        const uint32_t sel = gpio->func_out_sel_cfg[p->pad].func_sel;
        const uint32_t oen = (p->pad < 32) ? (gpio->enable >> p->pad) & 1U
                                           : (gpio->enable1.data >> (p->pad - 32)) & 1U;

        LOG_INF("wire: pad %2u %-4s out_sel=%3u (%s) oen=%u", p->pad, p->name, sel,
                (sel == p->sig) ? "as expected" : "NOT AS EXPECTED", oen);
    }

    /* 1-wire frame, then a quad frame, so the same registers can be compared
     * across the two line modes. The pixels land on the panel; the color test
     * that follows repaints the whole screen. */
    axs15231b_wire_measure(dev, &data->spi_cmd, &data->spi_cmd, AXS_OPCODE_WRITE_CMD, "1wire");
    axs15231b_wire_measure(dev, &data->spi_cmd, &data->spi_pixels, AXS_OPCODE_WRITE_COLOR, "quad");
}
#endif /* CONFIG_AXS15231B_WIRE_PROBE */

#if AXS_SPI_PATH && IS_ENABLED(CONFIG_AXS15231B_PAD_PROBE)
/*
 * Pad capture (bring-up aid, ESP32-S3).
 *
 * The registers prove the pixel phase leaves the controller on four lines, and
 * the GPIO matrix proves those four signals are routed to the four pads. The
 * one thing neither can show is the content on the pads themselves: that the
 * lines actually move, that CS really stays down across the whole frame, and
 * that each line carries the bytes the payload meant for it.
 *
 * So sample the pads while a transfer runs. The transfers are busy-polling, so
 * only something that preempts them can get a look in: a k_timer expiry runs in
 * the kernel timeout handler, and with a 100 kHz system tick one expiry is
 * 10 us. Run the pixel phase at 8 kHz - one clock every 125 us, about a dozen
 * samples per clock - and the recorded levels spell out the bits.
 *
 * 64 bytes is exactly one FIFO load, which in quad mode is exactly 16 clocks,
 * and the four byte lanes are four different counters, so the leading bits of
 * every clock identify which lane carried which byte.
 */
#include <hal/gpio_ll.h>     /* gpio_ll_input_enable */
#include <soc/gpio_struct.h> /* gpio_dev_t */

#define AXS_PP_CS 45
#define AXS_PP_SCLK 47
#define AXS_PP_D0 21
#define AXS_PP_D1 48
#define AXS_PP_D2 40
#define AXS_PP_D3 39

#define AXS_PP_HZ 8000 /* pixel clock: one clock every 125 us */
#define AXS_PP_BYTES 64        /* one FIFO load = 128 quad clocks */
#define AXS_PP_SAMPLES 256
#define AXS_PP_PERIOD_US 4
/* Cycles per microsecond, for the interval column. */
#define AXS_PP_CYCLES_PER_US (CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC / 1000000)

static const gpio_dev_t *axs_pp_gpio;
static uint8_t axs_pp_bits[AXS_PP_SAMPLES];
static uint16_t axs_pp_us[AXS_PP_SAMPLES];
static uint32_t axs_pp_last_cycles;
static uint8_t axs_pp_payload[AXS_PP_BYTES] __aligned(4);
static struct k_timer axs_pp_timer;
static volatile unsigned int axs_pp_n;

static uint8_t axs_pp_read(void) {
    const gpio_dev_t *g = axs_pp_gpio;
    uint8_t b = 0;

    b |= (uint8_t)((g->in1.data >> (AXS_PP_CS - 32)) & 1U);              /* bit 0 */
    b |= (uint8_t)(((g->in1.data >> (AXS_PP_SCLK - 32)) & 1U) << 1);     /* bit 1 */
    b |= (uint8_t)(((g->in >> AXS_PP_D0) & 1U) << 2);                    /* bit 2 */
    b |= (uint8_t)(((g->in1.data >> (AXS_PP_D1 - 32)) & 1U) << 3);       /* bit 3 */
    b |= (uint8_t)(((g->in1.data >> (AXS_PP_D2 - 32)) & 1U) << 4);       /* bit 4 */
    b |= (uint8_t)(((g->in1.data >> (AXS_PP_D3 - 32)) & 1U) << 5);       /* bit 5 */

    return b;
}

static void axs_pp_tick(struct k_timer *timer) {
    if (axs_pp_n >= AXS_PP_SAMPLES) {
        return;
    }

    const uint32_t cycles = sys_clock_cycle_get_32();
    const uint32_t delta = cycles - axs_pp_last_cycles;

    axs_pp_last_cycles = cycles;
    axs_pp_us[axs_pp_n] = (uint16_t)(delta / AXS_PP_CYCLES_PER_US);
    axs_pp_bits[axs_pp_n++] = axs_pp_read();
}

/* '#' = high, '.' = low. Reading the six lines column-wise gives one clock. */
static void axs_pp_dump(const char *name, unsigned int bit) {
    static char line[AXS_PP_SAMPLES + 1];

    for (size_t i = 0; i < AXS_PP_SAMPLES; i++) {
        line[i] = (axs_pp_bits[i] & BIT(bit)) ? '#' : '.';
    }
    line[AXS_PP_SAMPLES] = '\0';

    LOG_INF("pad %-5s %s", name, line);
}

/*
 * Microseconds between samples. The timer asks for one tick but the interval is
 * what it actually was, and the decode depends on that: the SCLK waveform is
 * only meaningful once the sample spacing is known.
 */
static void axs_pp_dump_intervals(void) {
    static char line[64 * 6 + 1];
    size_t at = 0;

    for (size_t i = 0; i < AXS_PP_SAMPLES; i += 64) {
        at = 0;
        for (size_t j = i; j < i + 64 && j < AXS_PP_SAMPLES; j++) {
            at += snprintk(&line[at], 8, "%u,", (unsigned int)axs_pp_us[j]);
        }
        LOG_INF("pad us   @%zu %s", i, line);
    }
}

static void axs15231b_pad_probe(const struct device *dev) {
    const struct axs15231b_cfg *cfg = dev->config;
    struct axs15231b_data *data = dev->data;
    gpio_dev_t *gpio = (gpio_dev_t *)DT_REG_ADDR(DT_NODELABEL(gpio0));

    /* The same two frames the panel gets, only slow enough to sample. */
    struct spi_config slow_cmd = data->spi_cmd;
    struct spi_config slow_quad = data->spi_pixels;
    uint8_t header[4] __aligned(4) = {
        AXS_OPCODE_WRITE_COLOR,
        0x00,
        AXS_CMD_RAMWR,
        0x00,
    };
    struct spi_buf header_buf = {.buf = header, .len = sizeof(header)};
    struct spi_buf_set header_set = {.buffers = &header_buf, .count = 1};
    struct spi_buf payload_buf = {.buf = axs_pp_payload, .len = sizeof(axs_pp_payload)};
    struct spi_buf_set payload_set = {.buffers = &payload_buf, .count = 1};

    slow_cmd.frequency = AXS_PP_HZ;
    slow_quad.frequency = AXS_PP_HZ;

    /* Four distinguishable byte lanes: leading 1 / 01 / 001 / 0001, then a
     * clock counter, so each line says which lane it is and which clock. */
    for (unsigned int k = 0; k < AXS_PP_BYTES / 4; k++) {
        axs_pp_payload[4 * k + 0] = (uint8_t)(0x80U + k);
        axs_pp_payload[4 * k + 1] = (uint8_t)(0x40U + k);
        axs_pp_payload[4 * k + 2] = (uint8_t)(0x20U + k);
        axs_pp_payload[4 * k + 3] = (uint8_t)(0x10U + k);
    }

    axs_pp_gpio = gpio;
    static const unsigned int axs_pp_pads[] = {AXS_PP_CS, AXS_PP_SCLK, AXS_PP_D0,
                                               AXS_PP_D1, AXS_PP_D2,   AXS_PP_D3};
    for (size_t i = 0; i < ARRAY_SIZE(axs_pp_pads); i++) {
        gpio_ll_input_enable(gpio, axs_pp_pads[i]);
    }

    axs_pp_n = 0;
    axs_pp_last_cycles = sys_clock_cycle_get_32();
    k_timer_init(&axs_pp_timer, axs_pp_tick, NULL);

    axs15231b_cs_select(dev);

    /* Sample across the header as well: it is a known 32-bit word, so the
     * capture says in plain terms which bit leaves on which line, in which
     * order and on which clock edge - the framing the whole panel depends on
     * and the one thing no register can show. */
    k_timer_start(&axs_pp_timer, K_NO_WAIT, K_USEC(AXS_PP_PERIOD_US));

    int err = spi_transceive(cfg->spi.bus, &slow_cmd, &header_set, NULL);
    int err2 = spi_transceive(cfg->spi.bus, &slow_quad, &payload_set, NULL);
    k_timer_stop(&axs_pp_timer);

    axs15231b_cs_release(dev);

    LOG_INF("pad: %u/%u samples, asking for %u us; clock %u Hz = %u us per cycle, %u us per bit",
            (unsigned int)axs_pp_n, (unsigned int)AXS_PP_SAMPLES, AXS_PP_PERIOD_US, AXS_PP_HZ,
            2000000U / AXS_PP_HZ, 1000000U / AXS_PP_HZ);
    LOG_INF("pad: header on the wire, MSB first: 0x32 0x00 0x2C 0x00 (write color, RAMWR)");
    LOG_INF("pad: payload bytes, memory order: 80+k 40+k 20+k 10+k (k = 0..15)");

    axs_pp_dump("CS", 0);
    axs_pp_dump("SCLK", 1);
    axs_pp_dump("D0", 2);
    axs_pp_dump("D1", 3);
    axs_pp_dump("D2", 4);
    axs_pp_dump("D3", 5);
    axs_pp_dump_intervals();

    if (err || err2) {
        LOG_ERR("pad: transfers failed: %d / %d", err, err2);
    }
}
#endif /* CONFIG_AXS15231B_PAD_PROBE */

#if IS_ENABLED(CONFIG_AXS15231B_I2C_PROBE)
/*
 * Is the controller alive? (bring-up aid)
 *
 * Everything measured so far is one-sided: the ESP32 says it pushed bytes, and
 * the glass says nothing. The AXS15231B offers a second, completely independent
 * window - its touch block answers on I2C at 0x3B (SDA GPIO4, SCL GPIO8, the
 * wiring this module uses), which is how the reference driver talks to it and
 * what the vendor demo on this very unit reads without a NACK.
 *
 * That traffic never touches the display bus, so it cannot be confounded by
 * anything in the QSPI setup. An ACK at 0x3B says the controller has power and
 * is running its own firmware; the reply to a touch request is real data out of
 * a live controller. Together they say whether the chip has come out of sleep
 * at all, which is the question every QSPI measurement so far has been unable
 * to answer.
 */
#include <zephyr/drivers/i2c.h>
#include <soc/gpio_struct.h> /* gpio_dev_t */
#include <zephyr/dt-bindings/pinctrl/esp32s3-gpio-sigmap.h>

#define AXS_I2C_SDA_PAD 4
#define AXS_I2C_SCL_PAD 8

/* The reference driver sends this before reading touch records: its header
 * 0xB5AB A55A, then the record length to come (8 bytes). */
static const uint8_t axs_i2c_touch_req[] = {
    0xB5, 0xAB, 0xA5, 0x5A, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00,
};

/*
 * Is the bus even moving? A bare scan cannot tell "no slave answered" from "no
 * clock came out", so watch the two pads while a transfer runs. The same
 * k_timer trick as the pad probe: the master transfer polls for completion with
 * the CPU, so only something that preempts it can look in, and a kernel timer
 * expiry does exactly that. At 400 kHz the clock period is 2.5 us, which the
 * sampling tick cannot resolve - but resolving it is not the question. Counting
 * how often each line is low over a window inside the transfer separates a
 * toggling clock (a working master with a silent slave) from two lines that
 * never move at all (a master, or its pin routing, that is not working).
 */
#define AXS_I2C_SAMPLES 128
static const gpio_dev_t *axs_i2c_gpio;
static uint8_t axs_i2c_bits[AXS_I2C_SAMPLES];
static volatile unsigned int axs_i2c_n;
static struct k_timer axs_i2c_timer;

static void axs_i2c_tick(struct k_timer *timer) {
    if (axs_i2c_n >= AXS_I2C_SAMPLES) {
        return;
    }

    uint8_t v = 0;

    if (axs_i2c_gpio->in & BIT(AXS_I2C_SDA_PAD)) {
        v |= BIT(0);
    }
    if (axs_i2c_gpio->in & BIT(AXS_I2C_SCL_PAD)) {
        v |= BIT(1);
    }
    axs_i2c_bits[axs_i2c_n++] = v;
}

static void axs_i2c_dump(const char *name, unsigned int bit) {
    static char line[AXS_I2C_SAMPLES + 1];
    unsigned int low = 0;

    for (size_t i = 0; i < AXS_I2C_SAMPLES; i++) {
        line[i] = (axs_i2c_bits[i] & BIT(bit)) ? '#' : '.';
        low += (axs_i2c_bits[i] & BIT(bit)) ? 0 : 1;
    }
    line[AXS_I2C_SAMPLES] = '\0';

    LOG_INF("i2c: %-3s %s (%u/%u samples low)", name, line, low, (unsigned int)AXS_I2C_SAMPLES);
}

static void axs15231b_i2c_bus_check(const struct device *bus, uint8_t addr) {
    gpio_dev_t *gpio = (gpio_dev_t *)DT_REG_ADDR(DT_NODELABEL(gpio0));
    uint8_t resp[64];
    int low_scl = 0;
    int low_sda = 0;

    axs_i2c_gpio = gpio;

    /* What the GPIO matrix routes these pads to, and whether their outputs are
     * enabled: a pinmux that never took effect looks identical to a dead slave
     * from the master's side. */
    LOG_INF("i2c: pad %d SDA out_sel=%u (%s) oen=%u in=%u", AXS_I2C_SDA_PAD,
            (unsigned int)gpio->func_out_sel_cfg[AXS_I2C_SDA_PAD].func_sel,
            gpio->func_out_sel_cfg[AXS_I2C_SDA_PAD].func_sel == ESP_I2CEXT0_SDA_OUT ? "as expected"
                                                                                    : "NOT AS EXPECTED",
            (unsigned int)((gpio->enable >> AXS_I2C_SDA_PAD) & 1U),
            (unsigned int)((gpio->in >> AXS_I2C_SDA_PAD) & 1U));
    LOG_INF("i2c: pad %d SCL out_sel=%u (%s) oen=%u in=%u", AXS_I2C_SCL_PAD,
            (unsigned int)gpio->func_out_sel_cfg[AXS_I2C_SCL_PAD].func_sel,
            gpio->func_out_sel_cfg[AXS_I2C_SCL_PAD].func_sel == ESP_I2CEXT0_SCL_OUT ? "as expected"
                                                                                    : "NOT AS EXPECTED",
            (unsigned int)((gpio->enable >> AXS_I2C_SCL_PAD) & 1U),
            (unsigned int)((gpio->in >> AXS_I2C_SCL_PAD) & 1U));

    axs_i2c_n = 0;
    k_timer_init(&axs_i2c_timer, axs_i2c_tick, NULL);
    k_timer_start(&axs_i2c_timer, K_NO_WAIT, K_USEC(4));

    /* One long read: 64 bytes at 400 kHz is ~1.4 ms, and the sampling window is
     * comfortably inside it. */
    int err = i2c_write_read(bus, addr, axs_i2c_touch_req, sizeof(axs_i2c_touch_req), resp,
                             sizeof(resp));

    k_timer_stop(&axs_i2c_timer);

    LOG_INF("i2c: 64 B read at 0x%02x -> err %d, %u samples taken", addr, err,
            (unsigned int)axs_i2c_n);

    for (size_t i = 0; i < axs_i2c_n; i++) {
        low_scl += (axs_i2c_bits[i] & BIT(1)) ? 0 : 1;
        low_sda += (axs_i2c_bits[i] & BIT(0)) ? 0 : 1;
    }

    LOG_INF("i2c: during the transfer SCL was low %u/%u, SDA low %u/%u -> %s", low_scl,
            (unsigned int)axs_i2c_n, low_sda, (unsigned int)axs_i2c_n,
            low_scl ? "master is clocking, the slave is silent"
                    : "no clock came out, the problem is on our side");

    axs_i2c_dump("SDA", 0);
    axs_i2c_dump("SCL", 1);
}

static void axs15231b_i2c_probe(void) {
    struct i2c_dt_spec touch = I2C_DT_SPEC_GET(DT_NODELABEL(axs15231b_touch));
    uint8_t resp[8] = {0};
    char acked[10 * 3 + 1];
    size_t at = 0;

    LOG_INF("i2c: probing %s for the controller at 0x%02x", touch.bus->name,
            (unsigned int)touch.addr);

    if (!i2c_is_ready_dt(&touch)) {
        /* The display driver does not depend on I2C in the device tree, so the
         * bus may still be waiting for its own init; give it a chance. */
        for (int try_n = 0; try_n < 5 && !i2c_is_ready_dt(&touch); try_n++) {
            k_msleep(100);
        }
    }

    if (!i2c_is_ready_dt(&touch)) {
        LOG_ERR("i2c: bus %s not ready", touch.bus->name);
        return;
    }

    /* Every address that ACKs a one-byte read, so the controller's absence can
     * be told from a wrong address, and a wrong address from a dead bus. */
    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
        uint8_t scratch = 0;

        if (i2c_read(touch.bus, &scratch, sizeof(scratch), addr) == 0 && at + 3 < sizeof(acked)) {
            at += snprintk(&acked[at], 4, "%02x ", addr);
        }
    }
    acked[at] = '\0';
    LOG_INF("i2c: addresses that ACK: %s", acked[0] ? acked : "(none)");

    /* The reference driver's read, so a live controller has something to say. */
    for (int round = 0; round < 3; round++) {
        int err = i2c_write_read_dt(&touch, axs_i2c_touch_req, sizeof(axs_i2c_touch_req), resp,
                                    sizeof(resp));

        LOG_INF("i2c: touch read %d: err %d, reply %02x %02x %02x %02x %02x %02x %02x %02x",
                round, err, resp[0], resp[1], resp[2], resp[3], resp[4], resp[5], resp[6], resp[7]);
        k_msleep(250);
    }

    axs15231b_i2c_bus_check(touch.bus, touch.addr);
}
#endif /* CONFIG_AXS15231B_I2C_PROBE */

#if AXS_SPI_PATH && IS_ENABLED(CONFIG_AXS15231B_CS_PROBE)
/*
 * Does the CS pad actually move?
 *
 * CS is GPIO45, which on the ESP32-S3 doubles as the VDD_SPI pin, and this
 * driver bit-bangs it as a plain GPIO output while the reference configuration
 * lets the SPI block drive it. Only the register side of that has ever been
 * confirmed: the pad's output select is SIG_GPIO_OUT_IDX with its output
 * enabled, and a capture during a transfer shows the pad low - which is what a
 * correctly asserted CS looks like, and also exactly what a CS that never goes
 * back up looks like.
 *
 * That distinction decides everything, because a QSPI slave counts bits from the
 * falling edge of CS and only resynchronises on a rising one: with CS stuck low
 * the panel would see one endless, permanently misaligned bit stream. Perfect
 * bytes, forever out of step, nothing understood - which is the symptom.
 *
 * So drive CS both ways and read the level back the only way that means
 * something: gpio_pin_get_dt() ends up reading GPIO.in/in1, which is the pad,
 * not what we asked the pad to be.
 */
static void axs15231b_cs_pad_check(const struct device *dev) {
    const struct axs15231b_cfg *cfg = dev->config;
    int err;

    /* The input path has to be on to read a pin that is driven as an output. */
    err = gpio_pin_configure_dt(&cfg->cs, GPIO_OUTPUT_INACTIVE | GPIO_INPUT);
    if (err) {
        LOG_ERR("cs: cannot reconfigure CS for readback: %d", err);
        return;
    }

    for (unsigned int active = 0; active <= 1; active++) {
        int got;

        gpio_pin_set_dt(&cfg->cs, active);
        k_msleep(2);
        got = gpio_pin_get_dt(&cfg->cs);

        LOG_INF("cs: pin %u driven %s -> pad reads %d (expected %u, active-low applied) %s",
                cfg->cs.pin, active ? "asserted" : "idle", got, active,
                got == (int)active ? "ok" : "MISMATCH");
    }

    /* Idle again, ready for the next transfer. */
    err = gpio_pin_configure_dt(&cfg->cs, GPIO_OUTPUT_INACTIVE);
    if (err) {
        LOG_ERR("cs: cannot restore CS as an output: %d", err);
    }
}
#endif /* CONFIG_AXS15231B_CS_PROBE */

#if IS_ENABLED(CONFIG_AXS15231B_GPIO_QSPI_PROBE)
/*
 * Bit-bang the QSPI frames by hand (bring-up aid, ESP32-S3).
 *
 * Every layer above the wire has been compared against the driver that works on
 * this very module, and the panel still acknowledges nothing - so the panel is
 * now known to be alive (it still shows the image the reference demo painted into
 * it, across every reflash of ours) and to have been changed by nothing our
 * firmware has ever sent, including a hand-written command sequence.
 *
 * That leaves two explanations, and they are told apart by whether the hand-built
 * frames were ever really on the pads. The SPI block is out of the way, but its
 * absence has to be *measured*, not assumed: five of the six pads (CS 45, SCLK
 * 47, D1 48, D2 40, D3 39) live in the S3's second GPIO bank, and every claim so
 * far about them came from the GPIO driver or from SPI2, never from these
 * register writes. So the probe first drives each pad both ways and reads the
 * level back from the pad's input register, then watches the pad while the clock
 * runs, and only then talks to the panel.
 *
 * What it then sends is the reference driver's own framing - header on one line,
 * pixels on four, no RASET, one unbroken pixel stream per fill - as three whole
 * screens of a solid colour, a second and a half apart. The glass should end up
 * blue: red or green means a later fill did not land, and no colour at all is the
 * first honest negative this panel has been given.
 *
 * Note what this cannot test: the bit is set up before both clock edges, so the
 * two CPHA conventions look identical to the panel and only the idle level of
 * SCLK differs. Everything else - bit order, header layout, CS framing, window
 * addressing - is what the driver sends.
 */
#include <hal/gpio_ll.h>      /* gpio_ll_input_enable */
#include <soc/gpio_sig_map.h> /* SIG_GPIO_OUT_IDX */
#include <soc/gpio_struct.h>  /* gpio_dev_t */

#define AXS_GPIO_CS 45
#define AXS_GPIO_SCLK 47
#define AXS_GPIO_D0 21
#define AXS_GPIO_D1 48
#define AXS_GPIO_D2 40
#define AXS_GPIO_D3 39
#define AXS_GPIO_HALF_US 5 /* half-clock period: 100 kHz, and leisurely */

static const uint8_t axs_gpio_pads[] = {
    AXS_GPIO_CS,   AXS_GPIO_SCLK, AXS_GPIO_D0,
    AXS_GPIO_D1,   AXS_GPIO_D2,   AXS_GPIO_D3,
};

static const uint8_t axs_gpio_data_pads[] = {
    AXS_GPIO_D0,
    AXS_GPIO_D1,
    AXS_GPIO_D2,
    AXS_GPIO_D3,
};

static gpio_dev_t *axs_gpio;
static unsigned int axs_gpio_half_us = AXS_GPIO_HALF_US;
static bool axs_gpio_mode3 = true;
static bool axs_gpio_lanes_rev;

/* Five of the six pads (CS 45, SCLK 47, D1 48, D2 40, D3 39) are GPIO32..48,
 * which the S3 keeps in a second set of registers, numbered from 0 there - so
 * every access has to pick a bank. */
static void axs_gpio_take(unsigned int pad) {
    /* Own the pad outright: out through the GPIO matrix, output enabled. The
     * SPI block's signals stop reaching the pad the moment its out_sel is
     * SIG_GPIO_OUT_IDX, which is the same trick the pinctrl driver uses. */
    axs_gpio->func_out_sel_cfg[pad].func_sel = SIG_GPIO_OUT_IDX;
    axs_gpio->func_out_sel_cfg[pad].oen_sel = 0;
    axs_gpio->func_out_sel_cfg[pad].oen_inv_sel = 0;

    if (pad >= 32) {
        axs_gpio->enable1_w1ts.val = BIT(pad - 32);
    } else {
        axs_gpio->enable_w1ts = BIT(pad);
    }
}

static void axs_gpio_set(unsigned int pad, unsigned int level) {
    if (pad >= 32) {
        if (level) {
            axs_gpio->out1_w1ts.val = BIT(pad - 32);
        } else {
            axs_gpio->out1_w1tc.val = BIT(pad - 32);
        }
    } else if (level) {
        axs_gpio->out_w1ts = BIT(pad);
    } else {
        axs_gpio->out_w1tc = BIT(pad);
    }
}

/* The pad, not the latch: this is what the panel sees. Needs the input path on,
 * which gpio_ll_input_enable does with the global pad number. */
static unsigned int axs_gpio_read(unsigned int pad) {
    if (pad >= 32) {
        return (axs_gpio->in1.data >> (pad - 32)) & 1U;
    }

    return (axs_gpio->in >> pad) & 1U;
}

static void axs_gpio_idle(bool mode3) {
    axs_gpio_mode3 = mode3;
    axs_gpio_set(AXS_GPIO_CS, 1);
    axs_gpio_set(AXS_GPIO_SCLK, mode3 ? 1 : 0);
    for (size_t i = 0; i < ARRAY_SIZE(axs_gpio_data_pads); i++) {
        axs_gpio_set(axs_gpio_data_pads[i], 1);
    }
}

/*
 * Do the pads actually move?
 *
 * Drives all six low then high and reads each back from the pad, which is the
 * only way to tell "this register write drives the pin" from "the pad is still
 * somewhere else entirely". A mismatch here invalidates every bit-banged frame
 * that follows, so this runs first and prints its answer in full.
 */
static void axs_gpio_pad_check(const char *tag) {
    char got[ARRAY_SIZE(axs_gpio_pads) * 2 + 1];
    char want[ARRAY_SIZE(axs_gpio_pads) * 2 + 1];
    size_t at;

    for (unsigned int level = 0; level <= 1; level++) {
        for (size_t i = 0; i < ARRAY_SIZE(axs_gpio_pads); i++) {
            axs_gpio_set(axs_gpio_pads[i], level);
        }
        k_busy_wait(20);

        at = 0;
        for (size_t i = 0; i < ARRAY_SIZE(axs_gpio_pads); i++) {
            got[at] = axs_gpio_read(axs_gpio_pads[i]) ? '#' : '.';
            want[at] = level ? '#' : '.';
            at++;
        }
        got[at] = '\0';
        want[at] = '\0';

        LOG_INF("gpio qspi: %s pads driven %s: want %s, pad says %s %s", tag,
                level ? "high" : "low", want, got, strcmp(got, want) == 0 ? "ok" : "MISMATCH");
    }
}

/*
 * ...and does it move like a clock?
 *
 * A static level proves the drive and the bank, not that a stream of edges
 * reaches the pin, so watch SCLK from the pad while a frame is hand-clocked
 * through it. The sampling is a kernel timer, as in the other probes: it preempts
 * the bit-banging thread, so it can look in while the frame runs. The clock here
 * is slowed to 25 kHz so a ~4 us sample lands several times inside each period.
 */
#define AXS_GPIO_CLK_SAMPLES 96
#define AXS_GPIO_NOP         0x00 /* DCS NOP: nothing to do, but it still clocks out */
static uint8_t axs_gpio_clk[AXS_GPIO_CLK_SAMPLES];
static volatile unsigned int axs_gpio_clk_n;
static struct k_timer axs_gpio_clk_timer;

static void axs_gpio_frame(uint8_t opcode, uint8_t cmd, const uint8_t *params, size_t len);

static void axs_gpio_clk_tick(struct k_timer *timer) {
    if (axs_gpio_clk_n < AXS_GPIO_CLK_SAMPLES) {
        axs_gpio_clk[axs_gpio_clk_n++] = (uint8_t)axs_gpio_read(AXS_GPIO_SCLK);
    }
}

static void axs_gpio_clock_check(void) {
    static char line[AXS_GPIO_CLK_SAMPLES + 1];
    unsigned int transitions = 0;
    unsigned int n;

    axs_gpio_half_us = 20; /* a 25 kHz clock, slow enough to sample cleanly */

    k_timer_init(&axs_gpio_clk_timer, axs_gpio_clk_tick, NULL);
    axs_gpio_clk_n = 0;
    k_timer_start(&axs_gpio_clk_timer, K_NO_WAIT, K_USEC(4));
    axs_gpio_frame(AXS_OPCODE_WRITE_CMD, AXS_GPIO_NOP, NULL, 0);
    k_timer_stop(&axs_gpio_clk_timer);

    n = axs_gpio_clk_n;
    for (unsigned int i = 0; i < n; i++) {
        line[i] = axs_gpio_clk[i] ? '#' : '.';
        if (i > 0 && axs_gpio_clk[i] != axs_gpio_clk[i - 1]) {
            transitions++;
        }
    }
    line[n] = '\0';

    LOG_INF("gpio qspi: SCLK at the pad during 8 clocks: %u samples, %u transitions %s", n,
            transitions, line);
    LOG_INF("gpio qspi: many transitions means the clock really is at the pin; 0 or 1 means it "
            "is not, and nothing sent after this can be trusted");

    axs_gpio_half_us = AXS_GPIO_HALF_US;
}

/*
 * One clock. The data is written first and the clock then moves, so the slave
 * finds the bit set up for the edge it samples on: mode 3 launches on the
 * falling edge and samples on the rising one, mode 0 and mode 2 on the other
 * pair. SCLK is left at its idle level afterwards either way.
 */
static void axs_gpio_clock(void) {
    const bool idle_high = axs_gpio_mode3;

    axs_gpio_set(AXS_GPIO_SCLK, !idle_high); /* the launch edge */
    k_busy_wait(axs_gpio_half_us);
    axs_gpio_set(AXS_GPIO_SCLK, idle_high);  /* the sample edge */
    k_busy_wait(axs_gpio_half_us);
}

static void axs_gpio_bit(unsigned int d0) {
    axs_gpio_set(AXS_GPIO_D0, d0);
    axs_gpio_clock();
}

static void axs_gpio_byte(uint8_t b) {
    for (int i = 7; i >= 0; i--) {
        axs_gpio_bit((b >> i) & 1U);
    }
}

/*
 * Four lines at a time. The bit stream is the same MSB-first one; four bits go
 * out per clock, and which line carries which of them is exactly the thing under
 * test: the flash convention puts the more significant bit of the group on D0,
 * and axs_gpio_lanes_rev selects the opposite. A pixel stream whose group order
 * the panel does not share is still a stream of the right length - it just decodes
 * as scrambled values, which on a flat colour looks like noise rather than colour.
 */
static void axs_gpio_nibble(uint8_t n) {
    if (axs_gpio_lanes_rev) {
        axs_gpio_set(AXS_GPIO_D0, n & 1U);
        axs_gpio_set(AXS_GPIO_D1, (n >> 1) & 1U);
        axs_gpio_set(AXS_GPIO_D2, (n >> 2) & 1U);
        axs_gpio_set(AXS_GPIO_D3, (n >> 3) & 1U);
    } else {
        axs_gpio_set(AXS_GPIO_D0, (n >> 3) & 1U);
        axs_gpio_set(AXS_GPIO_D1, (n >> 2) & 1U);
        axs_gpio_set(AXS_GPIO_D2, (n >> 1) & 1U);
        axs_gpio_set(AXS_GPIO_D3, n & 1U);
    }
    axs_gpio_clock();
}

/* One QSPI frame under one CS assertion: the 32-bit header on D0, then payload. */
static void axs_gpio_frame(uint8_t opcode, uint8_t cmd, const uint8_t *params, size_t len) {
    axs_gpio_set(AXS_GPIO_CS, 0);
    k_busy_wait(axs_gpio_half_us);

    axs_gpio_byte(opcode);
    axs_gpio_byte(0x00);
    axs_gpio_byte(cmd);
    axs_gpio_byte(0x00);

    for (size_t i = 0; i < len; i++) {
        axs_gpio_byte(params[i]);
    }

    k_busy_wait(axs_gpio_half_us);
    axs_gpio_set(AXS_GPIO_CS, 1);
    k_busy_wait(axs_gpio_half_us);
}

/* One screen of RGB565: 320 x 480 x 2, streamed from a small repeating pattern in
 * RAM, in one frame, under one CS assertion. */
#define AXS_GPIO_FILL_BYTES (AXS_WIDTH * AXS_HEIGHT * 2)
#define AXS_GPIO_CHUNK      512

/* One bar of the striped pattern is AXS_GPIO_BAR_ROWS rows of one colour, and the
 * pattern that repeats is a light bar followed by a black one. */
#define AXS_GPIO_BAR_ROWS  4
#define AXS_GPIO_BAR_BYTES (AXS_WIDTH * AXS_GPIO_BAR_ROWS * 2)
#define AXS_GPIO_BARS      (2 * AXS_GPIO_BAR_BYTES)

/*
 * The whole conversation, in one framing: wake the controller, set the color
 * format, address a band of rows, fill it, turn the display on. If this framing
 * is understood, the band is visible; if it is not, those rows stay as they were.
 */
/*
 * One whole fill: a single CS assertion, the header, then the entire pixel
 * stream without a break.
 *
 * This is not a detail. The AXS15231B restarts its window pointer when RAMWR
 * (0x2C) is issued - which is why the reference driver switches to RAMWRC (0x3C,
 * "continue writing") for every flush that does not start at row 0. So a fill
 * chopped into several frames, each with its own RAMWR under its own CS, writes
 * the same first pixels over and over and never reaches the rest of the window:
 * an earlier run of this probe did exactly that, which is why its negative result
 * meant nothing. Pixels also have to be a single unbroken stream between them, so
 * the CS stays down for the whole fill.
 *
 * The pattern is a small repeated block, because the fill is one flat color: the
 * whole 320x480x2 window streams from 512 bytes of RAM.
 */
static void axs_gpio_fill(const char *name, uint8_t opcode, uint8_t ram_cmd,
                          const uint8_t *pattern, size_t pattern_len, size_t total_bytes) {
    uint32_t start = k_uptime_get_32();

    axs_gpio_set(AXS_GPIO_CS, 0);
    k_busy_wait(axs_gpio_half_us);

    axs_gpio_byte(opcode);
    axs_gpio_byte(0x00);
    axs_gpio_byte(ram_cmd);
    axs_gpio_byte(0x00);

    for (size_t sent = 0; sent < total_bytes;) {
        size_t n = MIN(pattern_len, total_bytes - sent);

        if (opcode == AXS_OPCODE_WRITE_COLOR) {
            for (size_t i = 0; i + 1 < n; i += 2) {
                axs_gpio_nibble(pattern[i] >> 4);
                axs_gpio_nibble(pattern[i] & 0xF);
                axs_gpio_nibble(pattern[i + 1] >> 4);
                axs_gpio_nibble(pattern[i + 1] & 0xF);
            }
        } else {
            for (size_t i = 0; i < n; i++) {
                axs_gpio_byte(pattern[i]);
            }
        }

        sent += n;
    }

    k_busy_wait(axs_gpio_half_us);
    axs_gpio_set(AXS_GPIO_CS, 1);
    k_busy_wait(axs_gpio_half_us);

    LOG_INF("gpio qspi: %s: %u B in one frame (%d-wire, %s lanes, %u us per half clock) in %u ms",
            name, (unsigned int)total_bytes, opcode == AXS_OPCODE_WRITE_COLOR ? 4 : 1,
            axs_gpio_lanes_rev ? "reversed" : "D0=first", axs_gpio_half_us,
            k_uptime_get_32() - start);
}

static void axs15231b_gpio_qspi_probe(void) {
    static uint8_t white[AXS_GPIO_CHUNK];
    static uint8_t checker[AXS_GPIO_CHUNK];
    static uint8_t bars[AXS_GPIO_BARS];
    static const uint8_t colmod_565 = 0x55;

    memset(white, 0xFF, sizeof(white));

    /* Pixel off, pixel on, pixel off, ... With 320 pixels to a row, an even
     * count, this repeats cleanly from one row to the next, so a correct decode
     * is a true checkerboard. */
    for (size_t i = 0; i < sizeof(checker); i += 4) {
        checker[i] = 0x00;
        checker[i + 1] = 0x00;
        checker[i + 2] = 0xFF;
        checker[i + 3] = 0xFF;
    }

    /* A light bar then a dark one, each AXS_GPIO_BAR_ROWS rows tall. */
    memset(bars, 0xFF, AXS_GPIO_BAR_BYTES);
    memset(&bars[AXS_GPIO_BAR_BYTES], 0x00, AXS_GPIO_BAR_BYTES);

    axs_gpio = (gpio_dev_t *)DT_REG_ADDR(DT_NODELABEL(gpio0));

    LOG_INF("gpio qspi: taking the six pads over from SPI2");
    for (size_t i = 0; i < ARRAY_SIZE(axs_gpio_pads); i++) {
        axs_gpio_take(axs_gpio_pads[i]);
        gpio_ll_input_enable(axs_gpio, axs_gpio_pads[i]);
    }

    /* Everything the frames depend on, measured before any of them are sent. */
    axs_gpio_pad_check("idle");
    axs_gpio_idle(true);
    axs_gpio_clock_check();
    axs_gpio_pad_check("after the clock");

    /*
     * Init exactly as the reference driver does it, including what it leaves out:
     * no RASET at all, because in QSPI mode the panel derives the rows from the
     * write command and the number of pixels streamed, and the reference guards
     * RASET with `if (0 == flags.use_qspi_interface)` for that reason.
     */
    axs_gpio_frame(AXS_OPCODE_WRITE_CMD, AXS_CMD_SWRESET, NULL, 0);
    k_msleep(120);
    axs_gpio_frame(AXS_OPCODE_WRITE_CMD, AXS_CMD_SLPOUT, NULL, 0);
    k_msleep(120);
    axs_gpio_frame(AXS_OPCODE_WRITE_CMD, AXS_CMD_COLMOD, &colmod_565, 1);
    axs_gpio_frame(AXS_OPCODE_WRITE_CMD, AXS_CMD_CASET, (const uint8_t[]){0x00, 0x00, 0x01, 0x3F},
                   4);
    /* The software reset turned the display off; nothing painted after this would
     * be seen without switching it back on. */
    axs_gpio_frame(AXS_OPCODE_WRITE_CMD, AXS_CMD_DISPON, NULL, 0);
    k_msleep(20);

/*
 * Three whole screens in a row, chosen so that the lane order and the byte order of
 * each pixel cannot matter: 0xFFFF and 0x0000 look the same under every permutation
 * of the bits inside a pixel, so what appears on the glass is a verdict on the
 * framing alone, with the value mapping taken out of the question.
 *
 *   (1) whole screen 0xFFFF
 *   (2) pixel-on / pixel-off checkerboard
 *   (3) light and dark bars, AXS_GPIO_BAR_ROWS rows each
 *
 * What each outcome means. Solid white for (1) and clean horizontal bars for (3)
 * mean the stream arrives as pixels, 320 of them to a row, two bytes each - so the
 * noise seen earlier was only the value mapping, and the framing is settled.
 * Diagonal skew means the panel is taking a different number of bytes per pixel
 * (RGB666 or 888 rather than 565) or a different row width, which is the usual
 * cause of this kind of noise. Solid black or white throughout means these bytes
 * are not becoming pixels at all, and the noise is something else.
 */
    axs_gpio_half_us = 0; /* as fast as bit-banged GPIO goes */
    axs_gpio_lanes_rev = false;

    LOG_INF("gpio qspi: 1 of 3: whole screen 0xFFFF");
    axs_gpio_fill("white", AXS_OPCODE_WRITE_COLOR, AXS_CMD_RAMWR, white, sizeof(white),
                  AXS_GPIO_FILL_BYTES);
    k_msleep(2500);

    LOG_INF("gpio qspi: 2 of 3: pixel-on pixel-off checkerboard");
    axs_gpio_fill("checker", AXS_OPCODE_WRITE_COLOR, AXS_CMD_RAMWR, checker, sizeof(checker),
                  AXS_GPIO_FILL_BYTES);
    k_msleep(2500);

    LOG_INF("gpio qspi: 3 of 3: %u-row light and dark bars", AXS_GPIO_BAR_ROWS);
    axs_gpio_fill("bars", AXS_OPCODE_WRITE_COLOR, AXS_CMD_RAMWR, bars, sizeof(bars),
                  AXS_GPIO_FILL_BYTES);

    LOG_INF("gpio qspi: left on the glass: %u-row bars if the stream decodes as pixels at "
            "320 per row and 2 bytes each; diagonal skew instead means the panel takes a "
            "different number of bytes per pixel or a different row width; anything else "
            "means these are not becoming pixels", AXS_GPIO_BAR_ROWS);
    LOG_INF("gpio qspi: done; SPI2 no longer reaches these pads, so nothing else can change "
            "the glass afterwards");
}
#endif /* CONFIG_AXS15231B_GPIO_QSPI_PROBE */

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
        /*
         * Software reset. The bus's phantom transfer that used to make this
         * command do double duty is gone (it only existed while the SPI core
         * owned CS), so one SWRESET is enough.
         */
        err = axs15231b_tx_cmd(dev, AXS_CMD_SWRESET, NULL, 0);
        if (err) {
            LOG_ERR("SWRESET failed: %d", err);
            return err;
        }
        k_msleep(AXS_SWRESET_DELAY_MS);
    }

    /*
     * Wake the panel before configuring anything. After a power-on reset the
     * AXS15231B is asleep with the display off, and the vendor-specific
     * commands of axs15231b_init_cmds (boost converter, gate and source
     * drivers, gamma) do not take effect while it is: a panel configured
     * while asleep keeps the backlight lit over a permanently dark screen,
     * and its touch block never answers on I2C.
     *
     * Same order as the reference driver (esp_lcd_axs15231b.c
     * panel_axs15231b_init): SLPOUT, MADCTL, COLMOD, then the vendor table.
     */
    err = axs15231b_tx_cmd(dev, AXS_CMD_SLPOUT, NULL, 0);
    if (err) {
        LOG_ERR("SLPOUT failed: %d", err);
        return err;
    }
    k_msleep(AXS_SLPOUT_DELAY_MS);

    uint8_t madctl = 0x00; /* no swap, no mirror */
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
     * Bring the display up, exactly as the reference does on
     * esp_lcd_panel_disp_on_off(panel, false): exit sleep, then display on.
     * (The table's own trailing SLPOUT is not enough on its own - the display
     * still has to be switched on.)
     */
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

    data->initialized = true;

    LOG_INF("AXS15231B initialized (%ux%u)", AXS_WIDTH, AXS_HEIGHT);

#if AXS_SPI_PATH && IS_ENABLED(CONFIG_AXS15231B_WIRE_PROBE)
    axs15231b_wire_probe(dev);
#endif

#if AXS_SPI_PATH && IS_ENABLED(CONFIG_AXS15231B_PAD_PROBE)
    axs15231b_pad_probe(dev);
#endif

#if AXS_SPI_PATH && IS_ENABLED(CONFIG_AXS15231B_CS_PROBE)
    axs15231b_cs_pad_check(dev);
#endif

#if IS_ENABLED(CONFIG_AXS15231B_I2C_PROBE)
    axs15231b_i2c_probe();
#endif

#if IS_ENABLED(CONFIG_AXS15231B_COLOR_TEST)
    int test_err = axs15231b_color_test(dev);
    if (test_err) {
        LOG_ERR("Color test failed: %d", test_err);
        return test_err;
    }
    data->paint_hold = true;
#endif

#if IS_ENABLED(CONFIG_AXS15231B_FILL_TEST) || IS_ENABLED(CONFIG_AXS15231B_COMMAND_PROBE)
    /*
     * The fills and the visible commands have to run as one sequence on one
     * thread: each fill holds the bus for seconds, so anything else touching it
     * - a work item, or LVGL - would land in the middle of a frame. They repeat
     * for as long as the board is powered, so whoever is watching the glass can
     * look at any moment and catch a transition rather than the one ~35 s window
     * after power-on. paint_hold keeps LVGL off the bus while they run.
     */
    data->paint_hold = true;
    axs_bringup_start(dev);
#endif

#if IS_ENABLED(CONFIG_AXS15231B_GPIO_QSPI_PROBE)
    /* Last: it detaches the pads from SPI2, so nothing sent afterwards can
     * change what the panel has been shown. */
    axs15231b_gpio_qspi_probe();
#endif

#if AXS_SPI_PATH && IS_ENABLED(CONFIG_AXS15231B_QSPI_TOUCH_PROBE)
    axs_probe_start(dev);
#endif


    return 0;
}

static int axs15231b_write_wide(const struct device *dev, const uint16_t x, const uint16_t y,
                                const struct display_buffer_descriptor *desc, const void *buf,
                                bool wide) {
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
     * Program the column window (CASET). The row window (RASET) is deliberately
     * not sent: the AXS15231B in QSPI mode takes the rows from the write command
     * plus the number of pixels streamed, and the reference driver skips RASET for
     * exactly that reason (esp_lcd_axs15231b.c guards it with
     * `if (0 == flags.use_qspi_interface)`). LVGL flushes full-width bands from
     * the top down, so the panel's own row pointer lands on the right rows.
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
     * RAMWR (0x2C) starts a fresh write at the top of the window; RAMWRC (0x3C,
     * "continue writing") keeps the row pointer where the previous write of the
     * same frame left it. Which one the panel wants is decided by the row, as in
     * the reference driver.
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
        return axs15231b_tx_color_wide(dev, ram_cmd, buf, total_bytes, wide);
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

    err = axs15231b_tx_color_wide(dev, ram_cmd, tmp, total_bytes, wide);
    k_free(tmp);
    return err;
}

static int axs15231b_write(const struct device *dev, const uint16_t x, const uint16_t y,
                           const struct display_buffer_descriptor *desc, const void *buf) {
    struct axs15231b_data *data = dev->data;

#if IS_ENABLED(CONFIG_AXS15231B_COLOR_TEST) || IS_ENABLED(CONFIG_AXS15231B_FILL_TEST)
    /*
     * Only the display API is held off, never the write path itself: the fill
     * test calls axs15231b_write_wide() directly, and a hold that covered that
     * made every pattern "succeed" in milliseconds without sending anything.
     */
    if (data->paint_hold) {
        return 0;
    }
#endif

    return axs15231b_write_wide(dev, x, y, desc, buf,
                                 !IS_ENABLED(CONFIG_AXS15231B_PIXELS_1WIRE));
}

#if IS_ENABLED(CONFIG_AXS15231B_COLOR_TEST)
/*
 * Bring-up aid: paint the panel with four horizontal color bands at init, before
 * LVGL touches the display, so "does the pixel path reach the panel GRAM, and
 * does it land at the right rows?" can be answered without a working status
 * screen. Enabled with CONFIG_AXS15231B_COLOR_TEST.
 *
 * Bands, top to bottom: red, green, blue, white, each
 * AXS_COLOR_TEST_BAND_ROWS tall. Red and blue swapped means the RGB565 byte
 * order is wrong; a band at the wrong height means the window (CASET/RASET) is
 * not being applied.
 */
#define AXS_COLOR_TEST_BAND_ROWS (AXS_HEIGHT / 4)
#define AXS_COLOR_TEST_STRIP_ROWS 12

static int axs15231b_color_test(const struct device *dev) {
    static uint8_t strip[AXS_WIDTH * AXS_COLOR_TEST_STRIP_ROWS * 2] __aligned(4);
    const uint16_t band_colors[] = {
        0xF800, /* red */
        0x07E0, /* green */
        0x001F, /* blue */
        0xFFFF, /* white */
    };
    struct display_buffer_descriptor desc = {
        .buf_size = sizeof(strip),
        .width = AXS_WIDTH,
        .height = AXS_COLOR_TEST_STRIP_ROWS,
        .pitch = AXS_WIDTH,
    };

    for (size_t band = 0; band < ARRAY_SIZE(band_colors); band++) {
        const uint16_t color = band_colors[band];

        for (size_t i = 0; i < sizeof(strip); i += 2) {
            strip[i] = (uint8_t)(color >> 8); /* RGB565 is big-endian on the wire */
            strip[i + 1] = (uint8_t)(color & 0xFF);
        }

        for (uint16_t row = 0; row < AXS_COLOR_TEST_BAND_ROWS;
             row += AXS_COLOR_TEST_STRIP_ROWS) {
            const uint16_t y = (uint16_t)(band * AXS_COLOR_TEST_BAND_ROWS + row);
            const uint16_t rows =
                MIN((uint16_t)AXS_COLOR_TEST_STRIP_ROWS,
                    (uint16_t)(AXS_COLOR_TEST_BAND_ROWS - row));
            int err;

            desc.height = rows;
            err = axs15231b_write(dev, 0, y, &desc, strip);
            if (err) {
                LOG_ERR("Color test band %zu row %u failed: %d", band, y, err);
                return err;
            }
        }
    }

    LOG_INF("Color test: red/green/blue/white bands, %u rows each",
            (uint16_t)AXS_COLOR_TEST_BAND_ROWS);
    return 0;
}
#endif

#if IS_ENABLED(CONFIG_AXS15231B_FILL_TEST)
/*
 * Bring-up aid: the three mapping-invariant fills, sent through the same write
 * path LVGL uses, so a change of bus or framing can be judged by what appears on
 * the glass. Enabled with CONFIG_AXS15231B_FILL_TEST.
 *
 * Only 0xFFFF and 0x0000 are ever sent. Those two words look the same under
 * every permutation of the bits inside a pixel and under both byte orders, so
 * no color mapping can spoil the result: what appears is a verdict on the
 * framing alone - how many bytes make a pixel, how many pixels make a row, and
 * where the panel starts a row.
 *
 *   (1) whole panel on                          the one thing every framing agrees on
 *   (2) pixel-on / pixel-off checkerboard       diagonal skew means the bytes per
 *                                               pixel or the pixels per row are wrong
 *   (3) light and dark bars, 4 rows each        bars at the wrong height or slanted
 *                                               say where the rows actually landed
 *
 * Each fill restarts the window (the strip at y = 0 sends RAMWR, the ones below
 * it RAMWRC), and the fills are a few seconds apart so the transitions can be
 * watched one at a time. The bars, the last of the three, are the frame the
 * command probe then acts on.
 *
 * The set runs twice, the pixels on one data line and then on the bus's wide
 * phase, because the two failures look the same from the console and different
 * on the glass:
 *
 *   one line paints, wide does not  the framing and the commands are right,
 *                                   only the wide phase is broken
 *   neither paints                  nothing reaches GRAM; look at the command
 *                                   probe's visible commands
 *   both paint                      the wide phase is fine at this clock - the
 *                                   remaining step is to raise the clock
 */
#define AXS_FILL_TEST_STRIP_ROWS 16
#define AXS_FILL_TEST_BAR_ROWS 4
#define AXS_FILL_TEST_HOLD_MS 2000

static int axs15231b_fill_test(const struct device *dev) {
    static uint8_t strip[AXS_WIDTH * AXS_FILL_TEST_STRIP_ROWS * 2] __aligned(4);
    static const char *const pattern_names[] = {
        "all on",
        "checkerboard",
        "4-row bars",
    };
    const bool wide_passes[] = {false, true};
    struct display_buffer_descriptor desc = {
        .buf_size = sizeof(strip),
        .width = AXS_WIDTH,
        .pitch = AXS_WIDTH,
    };

    for (size_t pass = 0; pass < ARRAY_SIZE(wide_passes); pass++) {
        const bool wide = wide_passes[pass];
        const char *pass_name = wide ? "wide" : "1-wire";

        for (size_t pattern = 0; pattern < ARRAY_SIZE(pattern_names); pattern++) {
            const bool checker = (pattern == 1);
            const bool bars = (pattern == 2);
            int64_t started = k_uptime_get();
            int err;

            for (uint16_t row = 0; row < (uint16_t)AXS_HEIGHT; row += AXS_FILL_TEST_STRIP_ROWS) {
                const uint16_t rows = MIN((uint16_t)AXS_FILL_TEST_STRIP_ROWS,
                                          (uint16_t)(AXS_HEIGHT - row));
                size_t i = 0;

                for (uint16_t y = 0; y < rows; y++) {
                    const uint16_t screen_y = (uint16_t)(row + y);

                    for (uint16_t x = 0; x < (uint16_t)AXS_WIDTH; x++, i += 2) {
                        bool on = true;

                        if (checker) {
                            on = (((uint16_t)(screen_y + x)) & 1U) != 0U;
                        } else if (bars) {
                            on = (((uint16_t)(screen_y / AXS_FILL_TEST_BAR_ROWS)) & 1U) == 0U;
                        }

                        /* RGB565 goes out big-endian: high byte first. */
                        strip[i] = on ? 0xFFU : 0x00U;
                        strip[i + 1] = on ? 0xFFU : 0x00U;
                    }
                }

                desc.height = rows;
                err = axs15231b_write_wide(dev, 0, row, &desc, strip, wide);
                if (err) {
                    LOG_ERR("fill test %s/%s at row %u failed: %d", pass_name,
                            pattern_names[pattern], row, err);
                    return err;
                }
            }

            /*
             * The elapsed time is part of the measurement, not decoration: the
             * panel is 320 x 480 x 2 bytes = 2457600 bits, so a full-panel fill
             * cannot take less than that divided by (clock x data lines). At the
             * 1 MHz this is brought up at that is ~2460 ms on one line and ~610 ms
             * on four, and a fill that "succeeds" far below those numbers wrote
             * nothing at all.
             */
            LOG_INF("fill test [%s pixels]: %s painted in %lld ms", pass_name,
                    pattern_names[pattern], k_uptime_get() - started);
            k_msleep(AXS_FILL_TEST_HOLD_MS);
        }
    }

    return 0;
}
#endif

#if IS_ENABLED(CONFIG_AXS15231B_COMMAND_PROBE)
/*
 * Command probe (bring-up aid).
 *
 * The panel gives us no read-back, so the only way to find out whether a
 * command was ever understood is to pick commands whose effect is visible on the
 * glass and have a person watch. It runs after the fills, so what it acts on is
 * a frame this firmware painted:
 *
 *   MADCTL scan-axes on / off   - the bars turn through 90 degrees and back.
 *                                 Needs no pixel writes of its own, so it also
 *                                 works on a frame left there by another
 *                                 firmware.
 *   DISPLAY_INVERSION_ON / _OFF - flips the frame. On an all black screen this
 *                                 is the whole point: black turning white is
 *                                 something only a panel with its output
 *                                 switched on can do, so it separates "the
 *                                 pixels arrived as black" from "the output is
 *                                 off", which look identical otherwise.
 *
 * If nothing on the glass changes at any point, no command of ours has been
 * understood, and no amount of reading GRAM-dependent patterns back will tell
 * us more.
 */
#define AXS_CMD_PROBE_ROUNDS 2
#define AXS_CMD_PROBE_PERIOD_MS 1500

struct axs_cmd_probe_step {
    const char *what;
    uint8_t cmd;
    const uint8_t *param;
    uint8_t param_len;
};

static const uint8_t axs_cmd_madctl_mv[] = {0x80}; /* swap X/Y scan axes */
static const uint8_t axs_cmd_madctl_normal[] = {0x00};

/*
 * The three commands that stay here are the ones with an effect a person can
 * see without any assumption about what is in GRAM. DISPLAY_OFF/ON is left out
 * on purpose: a panel whose output is switched off and a panel whose GRAM is
 * all black are the same black screen, so with DISPLAY_OFF first, nothing
 * afterwards can be read. Inversion is the discriminator - it turns an all
 * black frame white, which only a panel with its output on can do.
 */
static const struct axs_cmd_probe_step axs_cmd_probe_seq[] = {
    {"MADCTL scan axes swapped (the bars should rotate 90 deg)", AXS_CMD_MADCTL, axs_cmd_madctl_mv,
     sizeof(axs_cmd_madctl_mv)},
    {"MADCTL scan axes normal (the bars should rotate back)", AXS_CMD_MADCTL, axs_cmd_madctl_normal,
     sizeof(axs_cmd_madctl_normal)},
    {"DISPLAY_INVERSION_ON (a black screen here means the frame arrived as black, so this "
     "should turn it WHITE)",
     0x21, NULL, 0},
    {"DISPLAY_INVERSION_OFF (should turn it back)", 0x20, NULL, 0},
};

#define AXS_CMD_PROBE_STEPS (ARRAY_SIZE(axs_cmd_probe_seq) * AXS_CMD_PROBE_ROUNDS)

/*
 * Sent from the bring-up thread, after the fills, so it acts on a frame this
 * firmware painted and can describe. A work-queue item would land in the middle
 * of a fill (each of which holds the bus for seconds) and put a second command
 * stream on a bus that has no arbitration for that.
 */
static void axs_cmd_probe_run(const struct device *dev) {
    for (size_t round = 0; round < AXS_CMD_PROBE_ROUNDS; round++) {
        for (size_t i = 0; i < ARRAY_SIZE(axs_cmd_probe_seq); i++) {
            const struct axs_cmd_probe_step *entry = &axs_cmd_probe_seq[i];
            uint16_t step = (uint16_t)(round * ARRAY_SIZE(axs_cmd_probe_seq) + i + 1);
            int err = axs15231b_tx_cmd(dev, entry->cmd, entry->param, entry->param_len);

            LOG_INF("Command probe %u/%u: %s (err %d) - watch the screen", step,
                    (uint16_t)AXS_CMD_PROBE_STEPS, entry->what, err);

            k_msleep(AXS_CMD_PROBE_PERIOD_MS);
        }
    }

    LOG_INF("Command probe finished (the panel should be back to its normal look)");
}
#endif

#if IS_ENABLED(CONFIG_AXS15231B_FILL_TEST) || IS_ENABLED(CONFIG_AXS15231B_COMMAND_PROBE)
/*
 * Bring-up sequence (bring-up aid).
 *
 * The fills and the visible commands run back to back on their own thread,
 * forever. Both halves need the bus to themselves for seconds at a time, so one
 * thread owns the sequence - a work item would land in the middle of a fill -
 * and the fills being repeated means the person reading the glass never has to
 * be watching the one ~35 s window after power-on: any transition they arrive
 * to is one of these.
 *
 * The order is deliberate. The fills ask "do pixels land?"; if they do not, the
 * only question left is whether any command landed either, and the probe answers
 * that with commands whose effect is visible without writing a pixel.
 */
#define AXS_BRINGUP_STACK_SIZE  4096
#define AXS_BRINGUP_PRIORITY    7
#define AXS_BRINGUP_GAP_MS      2000

static struct k_thread axs_bringup_thread;
K_THREAD_STACK_DEFINE(axs_bringup_stack, AXS_BRINGUP_STACK_SIZE);
static const struct device *axs_bringup_dev;

static void axs_bringup_cycle(void *unused1, void *unused2, void *unused3) {
    const struct device *dev = axs_bringup_dev;
    unsigned int cycle = 0;

    for (;;) {
        /*
         * The backlight is enabled here, and every cycle after this, because
         * nothing else reliably does it in time. ZMK's unblank (blanking_off,
         * the only caller that turns the backlight on) is a later POST_KERNEL
         * step, and a panel with an unlit backlight is indistinguishable from a
         * dead one.
         */
        (void)axs15231b_set_backlight(dev, true);

        cycle++;
        LOG_INF("Bring-up cycle %u: painting", cycle);

#if IS_ENABLED(CONFIG_AXS15231B_FILL_TEST)
        int err = axs15231b_fill_test(dev);
        if (err != 0) {
            LOG_ERR("Fill test failed: %d", err);
        }
#endif

#if IS_ENABLED(CONFIG_AXS15231B_COMMAND_PROBE)
        LOG_INF("Fills done; the commands that follow must visibly move or invert that pattern");
        axs_cmd_probe_run(dev);
#endif

        k_msleep(AXS_BRINGUP_GAP_MS);
    }
}

static void axs_bringup_start(const struct device *dev) {
    axs_bringup_dev = dev;

    k_thread_create(&axs_bringup_thread, axs_bringup_stack,
                    K_THREAD_STACK_SIZEOF(axs_bringup_stack), axs_bringup_cycle, NULL, NULL, NULL,
                    AXS_BRINGUP_PRIORITY, 0, K_NO_WAIT);
    k_thread_name_set(&axs_bringup_thread, "axs_bringup");
}
#endif

#if AXS_SPI_PATH && IS_ENABLED(CONFIG_AXS15231B_QSPI_TOUCH_PROBE)
/*
 * QSPI touch probe (bring-up aid).
 *
 * The AXS15231B's touch block is not on I2C: the reference driver's
 * touch_axs15231b_read_data() sends a magic 11-byte request over the *display*
 * QSPI bus, with no command phase, then reads back 2 + 6 * points bytes
 * (gesture, point count, then per point x_h/x_l/y_h/y_l). Reproduced here so the
 * controller can be asked something only it can answer: on a dead bus the reply
 * is a constant, on a live one coordinates appear the moment a finger lands.
 */
#define AXS_PROBE_MAX_POINTS 2
#define AXS_PROBE_REPLY_LEN (AXS_PROBE_MAX_POINTS * 6 + 2)
#define AXS_PROBE_PERIOD_MS 500

static const uint8_t axs_probe_request[11] __aligned(4) = {
    0xB5, 0xAB, 0xA5, 0x5A, 0x00, 0x00, (uint8_t)(AXS_PROBE_REPLY_LEN >> 8),
    (uint8_t)(AXS_PROBE_REPLY_LEN & 0xFF), 0x00, 0x00, 0x00,
};

static uint8_t axs_probe_reply[AXS_PROBE_REPLY_LEN] __aligned(4);
static uint8_t axs_probe_previous[AXS_PROBE_REPLY_LEN];
static const struct device *axs_probe_dev;
static struct k_work_delayable axs_probe_work;

static int axs15231b_qspi_touch_read(const struct device *dev, uint8_t *reply) {
    const struct axs15231b_cfg *cfg = dev->config;
    struct spi_buf req_buf = {.buf = (void *)axs_probe_request, .len = sizeof(axs_probe_request)};
    struct spi_buf_set req = {.buffers = &req_buf, .count = 1};
    struct spi_buf rep_buf = {.buf = reply, .len = AXS_PROBE_REPLY_LEN};
    struct spi_buf_set rep = {.buffers = &rep_buf, .count = 1};
    int err;

    /*
     * Plain 1-wire, no QSPI opcode: the request is the payload, and the reply is
     * clocked in on D1 (that is why the D1 pad also carries the Q input signal).
     * CS is held down across request and reply, as for a color frame.
     */
    axs15231b_cs_select(dev);

    err = spi_transceive(cfg->spi.bus, &data->spi_cmd, &req, NULL);
    if (err) {
        axs15231b_cs_release(dev);
        return err;
    }

    err = spi_transceive(cfg->spi.bus, &data->spi_cmd, NULL, &rep);
    axs15231b_cs_release(dev);

    return err;
}

static void axs_probe_work_fn(struct k_work *work) {
    static uint8_t ticks;
    const struct device *dev = axs_probe_dev;
    bool changed;
    int err;

    err = axs15231b_qspi_touch_read(dev, axs_probe_reply);
    changed = memcmp(axs_probe_previous, axs_probe_reply, sizeof(axs_probe_reply)) != 0;

    if (err) {
        LOG_WRN("Touch probe failed: %d", err);
    } else if (changed) {
        memcpy(axs_probe_previous, axs_probe_reply, sizeof(axs_probe_previous));
        LOG_INF("Touch probe: points=%u x=%u y=%u", axs_probe_reply[1],
                (uint16_t)(((axs_probe_reply[2] & 0x0F) << 8) | axs_probe_reply[3]),
                (uint16_t)(((axs_probe_reply[4] & 0x0F) << 8) | axs_probe_reply[5]));
        LOG_HEXDUMP_INF(axs_probe_reply, sizeof(axs_probe_reply), "Touch probe raw");
    }

    /* Keep a heartbeat so a silent, constant reply is still distinguishable
     * from a probe that stopped running. */
    if (++ticks >= (4000 / AXS_PROBE_PERIOD_MS)) {
        ticks = 0;
        LOG_INF("Touch probe alive (reads every %u ms, last reply unchanged: %d)",
                AXS_PROBE_PERIOD_MS, !changed);
    }

    k_work_reschedule(&axs_probe_work, K_MSEC(AXS_PROBE_PERIOD_MS));
}

static void axs_probe_start(const struct device *dev) {
    axs_probe_dev = dev;
    k_work_init_delayable(&axs_probe_work, axs_probe_work_fn);
    k_work_schedule(&axs_probe_work, K_MSEC(AXS_PROBE_PERIOD_MS));

    LOG_INF("QSPI touch probe started (request + %u byte reply every %u ms)",
            (uint16_t)AXS_PROBE_REPLY_LEN, AXS_PROBE_PERIOD_MS);
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
    int err;

#if AXS_ON_MSPI
    if (!device_is_ready(cfg->mspi_bus)) {
        LOG_ERR("MSPI bus not ready for AXS15231B");
        return -ENODEV;
    }

    /*
     * Ask the controller what it can do before claiming it: this is the
     * documented order, it releases the controller's device-config lock for a
     * caller that left one behind, and -EINVAL from here means the node asks
     * for a channel the controller does not have.
     */
    err = mspi_get_channel_status(cfg->mspi_bus, 0);
    if (err) {
        LOG_ERR("MSPI channel 0 not usable for AXS15231B: %d", err);
        return err;
    }

    data->mspi_io_mode = MSPI_IO_MODE_MAX;

    /*
     * Everything that does not change per frame, in one call: the clock the
     * panel is rated for, the clock mode, and the CE line the controller drives
     * for this device. mspi_esp32.c came up at the controller node's own
     * clock-frequency, so the panel's rate has to be requested here.
     */
    err = mspi_dev_config(cfg->mspi_bus, &cfg->mspi_dev_id,
                          MSPI_DEVICE_CONFIG_FREQUENCY | MSPI_DEVICE_CONFIG_CPP |
                              MSPI_DEVICE_CONFIG_DATA_RATE | MSPI_DEVICE_CONFIG_CE_NUM |
                              MSPI_DEVICE_CONFIG_CE_POL | MSPI_DEVICE_CONFIG_ENDIAN,
                          &cfg->mspi_dev_cfg);
    if (err) {
        LOG_ERR("MSPI device configuration failed: %d", err);
        return err;
    }
#else
    if (!spi_is_ready_dt(&cfg->spi)) {
        LOG_ERR("SPI bus not ready for AXS15231B");
        return -ENODEV;
    }

    /*
     * The two line widths this panel needs, built once here and reused for the
     * lifetime of the driver: master mode, 8-bit words and the clock mode from
     * the devicetree, differing only in line mode.
     */
    const uint32_t base_op = SPI_OP_MODE_MASTER | SPI_WORD_SET(8) |
                             (cfg->spi.config.operation & (SPI_MODE_CPOL | SPI_MODE_CPHA));

    data->spi_cmd = cfg->spi.config;
    data->spi_cmd.operation = base_op;

    /*
     * Pixel data goes out on all four data lines. Requires
     * CONFIG_SPI_EXTENDED_MODES, and a zephyr tree carrying the "fix quad line
     * mode truncated to 16-bit" fix - see esp-port/patches/.
     */
    data->spi_pixels = data->spi_cmd;
    data->spi_pixels.operation |= SPI_LINES_QUAD;

    /*
     * The bus node deliberately has no cs-gpios (see the shield overlay), so the
     * SPI core never touches the line and this driver can hold CS across the
     * color header and its pixels. Tell the peripheral not to drive a hardware
     * CS either: cs_pin_id only honours 0-2 and any other value leaves every CS
     * line unused, so claim a slave number outside that range.
     */
    data->spi_cmd.slave = AXS_NO_HW_CS_SLAVE;
    data->spi_pixels.slave = AXS_NO_HW_CS_SLAVE;

    if (!gpio_is_ready_dt(&cfg->cs)) {
        LOG_ERR("CS GPIO not ready for AXS15231B");
        return -ENODEV;
    }

    err = gpio_pin_configure_dt(&cfg->cs, GPIO_OUTPUT_INACTIVE);
    if (err) {
        LOG_ERR("CS GPIO configure failed: %d", err);
        return err;
    }
#endif

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
#if AXS_ON_MSPI
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
        /* Bytes always leave MSB first on this controller; the panel expects
         * that, and it is what mspi_esp32.c does whatever this field says. */
        .endian = MSPI_XFER_BIG_ENDIAN,
        .ce_polarity = MSPI_CE_ACTIVE_LOW,
    },
#else
    .spi = SPI_DT_SPEC_GET(DT_INST(0, axs_axs15231b), SPI_OP_MODE_MASTER | SPI_WORD_SET(8)),
    .cs = GPIO_DT_SPEC_GET(DT_INST(0, axs_axs15231b), cs_gpios),
#endif
    .reset = GPIO_DT_SPEC_GET_OR(DT_INST(0, axs_axs15231b), reset_gpios, {0}),
#if AXS_HAS_BACKLIGHT
    .backlight = PWM_DT_SPEC_GET(DT_INST(0, axs_axs15231b)),
#endif
};

static struct axs15231b_data axs15231b_data;

DEVICE_DT_INST_DEFINE(0, axs15231b_init, NULL, &axs15231b_data, &axs15231b_cfg, POST_KERNEL,
                      CONFIG_DISPLAY_INIT_PRIORITY, &axs15231b_api);
