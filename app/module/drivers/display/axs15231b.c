/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * AXS15231B QSPI LCD panel driver (ESP32 MSPI controller).
 *
 * The AXS15231B is a 320x480 LCD controller with a QSPI interface, used on the
 * Guition JC3248W535 reference board and the cobantab1a module. Neither has an
 * RST or DC pin wired, so the panel is driven QSPI-only and reset through the
 * SWRESET command.
 *
 * QSPI wire protocol (AXS15231B datasheet, confirmed on the bench 2026-11-21):
 *   - Every frame opens with a 32-bit header on one data line:
 *       [opcode][0x00][cmd][0x00]
 *     where opcode is 0x02 (write command + parameters, 1-wire data phase) or
 *     0x32 (write color, pixels on all four data lines).
 *   - Header and payload are ONE transaction under one CS assertion. On this
 *     controller the header rides the 16-bit command phase ([opcode][0x00])
 *     and the 16-bit address phase ([cmd][0x00]), both 1-wire in every I/O
 *     mode, with the payload as the data phase - see axs15231b_mspi_frame().
 *   - The interface is SPI mode 3 (SCLK idles high, data latched on the rising
 *     edge), which on a GPIO-matrix clock pad needs the matrix inverter -
 *     see axs_mspi_clock_invert().
 *
 * The panel must be a child of an "espressif,esp32-mspi" node: this driver
 * exists to send header and pixels as a single hardware transaction, which the
 * SPI API (one line width per transfer, CS held by hand) cannot do.
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
 * Product orientation (a07 landscape redesign): `rotation` on the node is the
 * angle the CONTENT is turned by, so a portrait-mounted 320x480 panel shows a
 * 480x320 landscape UI. Two routes:
 *
 * - Software (default): the panel stays MADCTL 0x00, LVGL renders the logical
 *   landscape frame (lv_display_set_rotation, driven from the same DT
 *   property by app/src/display/main.c), and the flush arrives transposed -
 *   axs15231b_write transposes it into a PSRAM staging frame and streams it
 *   as one RAMWR, the same transaction shape the unrotated path uses. Costs
 *   a few ms of transpose per full frame; the 61 ms QSPI flush dominates.
 *
 * - Hardware (CONFIG_AXS15231B_ROTATION_HW): MADCTL's MV swap, caps report
 *   the swapped resolution, and the pixel stream is written straight through.
 *   ZERO per-frame cost - but the AXS15231B's QSPI row-pointer stream under
 *   MV is NOT yet proven on the bench (the columns-only-window findings in
 *   this file's notes were made at MADCTL 0x00), so the route is opt-in.
 *   The MADCTL table below is the MIPI-DDI convention and must be verified
 *   against a test pattern before this route ships; if the stream order comes
 *   out transposed, the software route is the answer.
 *
 * The software rotation direction is tied to LVGL's: the transpose formulas
 * are derived from lv_display_rotate_point() (lv_indev.c feeds touch through
 * the same map), so pixels and taps cannot drift apart. Keep them in lockstep
 * with any LVGL upgrade.
 */
#define AXS_ROTATION DT_PROP_OR(DT_INST(0, axs_axs15231b), rotation, 0)
#define AXS_ROTATION_HW IS_ENABLED(CONFIG_AXS15231B_ROTATION_HW)
#define AXS_ROTATION_SW (AXS_ROTATION != 0 && !AXS_ROTATION_HW)

BUILD_ASSERT(AXS_ROTATION == 0 || AXS_ROTATION == 90 || AXS_ROTATION == 180 ||
                     AXS_ROTATION == 270,
             "rotation: one of 0/90/180/270");

/* MADCTL bits per rotation (MX 0x80, MY 0x40, MV 0x20) - bench-verify first,
 * see the note above. */
#define AXS_MADCTL_ROT_0 0x00
#define AXS_MADCTL_ROT_90 0x60  /* MV | MX */
#define AXS_MADCTL_ROT_180 0xC0 /* MX | MY */
#define AXS_MADCTL_ROT_270 0xA0 /* MV | MY */

/* Logical (what LVGL/flush sees) vs physical (what the glass streams). Only
 * 90/270 swap the axes; the hardware route reports them in caps, the
 * software route keeps caps physical and transposes at write time. */
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

/* Where the pixel phase goes, from the node's mspi-io-mode. The binding keeps
 * its enum list in the order of enum mspi_io_mode so the index can be used as
 * the value. */
#define AXS_MSPI_PIXELS_IO_MODE                                                                          \
    ((enum mspi_io_mode)DT_ENUM_IDX_OR(DT_INST(0, axs_axs15231b), mspi_io_mode, MSPI_IO_MODE_SINGLE))

BUILD_ASSERT(AXS_MSPI_PIXELS_IO_MODE == MSPI_IO_MODE_SINGLE ||
                     AXS_MSPI_PIXELS_IO_MODE == MSPI_IO_MODE_QUAD_1_1_4,
             "mspi-io-mode: the header is always one line (command/address phases); the "
             "pixel phase is 1-1-1 or 1-1-4, nothing else");

struct axs15231b_cfg {
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
    struct gpio_dt_spec reset;
#if AXS_HAS_BACKLIGHT
    struct pwm_dt_spec backlight;
#endif
};

struct axs15231b_data {
    /*
     * The line width the controller is currently set to. Switching between the
     * 1-wire and 4-wire phases is a dev_config() call, and mspi_dev_config() is
     * not free (it takes the controller's lock and reprograms it), so it is
     * called only when the phase actually changes. MSPI_IO_MODE_MAX is not a
     * mode any device can ask for, so it starts the cache empty (set in
     * axs15231b_init(); a plain zero would look like MSPI_IO_MODE_SINGLE).
     */
    enum mspi_io_mode mspi_io_mode;

    bool initialized;
};

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
 * One frame as ONE hardware transaction: the 32-bit header
 * [opcode][0x00][cmd][0x00] split across the controller's command phase
 * (opcode, 0x00) and address phase (cmd, 0x00) - both on one data line in
 * every io_mode this panel uses - and the payload as the data phase, on one
 * line for parameters or the node's mspi-io-mode for pixels. One CS
 * assertion, no mid-frame mode switch, no boundary to lose sync at.
 *
 * This replaces an earlier form that sent the header as a 4-byte data-phase
 * transfer followed by a SEPARATE payload transfer held together by a
 * software-CS hold. If CS wavered at that seam the panel saw two broken
 * frames - a header with no payload, then a payload with no header whose
 * first bytes it parsed AS a header - and answered with pixel noise and
 * pixel bytes like 0x10 (SLEEP IN) decoded as commands. Bench-proven
 * 2026-11-21: this form paints from a virgin power-on.
 */
static int axs15231b_mspi_frame(const struct device *dev, uint8_t opcode, uint8_t cmd,
                                const void *payload, size_t payload_len,
                                enum mspi_io_mode io_mode) {
    const struct axs15231b_cfg *cfg = dev->config;
    struct mspi_xfer_packet packet = {
        .dir = MSPI_TX,
        /* Command and address phases go out MSB-first, so these pack the
         * wire bytes [opcode][0x00] and [cmd][0x00]. */
        .cmd = (uint32_t)opcode << 8,
        .address = (uint32_t)cmd << 8,
        .num_bytes = (uint32_t)payload_len,
        .data_buf = (uint8_t *)payload,
    };
    struct mspi_xfer xfer = {
        /* Payloads over a FIFO's worth ride GDMA; command/parameter frames
         * stay on PIO. */
        .xfer_mode = payload_len > 32U ? MSPI_DMA : MSPI_PIO,
        .cmd_length = 2,
        .addr_length = 2,
        .hold_ce = false,
        .packets = &packet,
        .num_packet = 1,
        /*
         * The controller's default timeout (its transfer-timeout, 100 ms
         * unless the node says otherwise) is sized for flash reads. A panel
         * frame is much longer at a low clock: a whole 320x480 RGB565 frame
         * is 300 kB, over 2 s at 1 MHz. So ask for twice the transfer's own
         * duration, in case the clock divider rounded down.
         */
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

/*
 * Send the whole command sequence that takes the panel from reset to
 * displaying, and return once it is up.
 *
 * The order is the reference's, and was read off it rather than assumed: reset,
 * then SLPOUT, MADCTL and COLMOD (panel_axs15231b_reset() and the head of
 * panel_axs15231b_init()), then the 32-entry vendor table, then SLPOUT and
 * DISPLAY_ON, which is what esp_lcd_panel_disp_on_off(panel, false) sends after
 * init. The table itself is evaluated untouched - it is panel-tuned and differs
 * from the driver's built-in default.
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

    /*
     * Wake the panel before configuring anything. After a power-on reset the
     * AXS15231B is asleep with the display off, and the vendor-specific
     * commands of axs15231b_init_cmds (boost converter, gate and source
     * drivers, gamma) do not take effect while it is: a panel configured
     * while asleep keeps the backlight lit over a permanently dark screen,
     * and its touch block never answers on I2C.
     */
    err = axs15231b_tx_cmd(dev, AXS_CMD_SLPOUT, NULL, 0);
    if (err) {
        LOG_ERR("SLPOUT failed: %d", err);
        return err;
    }
    k_msleep(AXS_SLPOUT_DELAY_MS);

#if AXS_ROTATION_HW
    /* Hardware route: see the rotation note at the top of the file - the
     * table needs a bench pass before this route ships. */
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

    return 0;
}

#if AXS_ROTATION_SW
/*
 * Software rotation: LVGL renders the logical landscape frame (see main.c's
 * lv_display_set_rotation) and the flush arrives as one full logical frame,
 * e.g. 480x320 for a 320x480 panel at 90/270. Transpose it into physical
 * strips here and stream those - the panel never learns the difference.
 *
 * Direction follows lv_display_rotate_point() (which is what maps touch):
 *   90:  physical P[y][x] = logical L[x][PH-1-y]
 *   270: P[y][x] = L[PW-1-x][y]
 *   180: P[y][x] = L[PH-1-y][PW-1-x]
 * PW/PH are the panel's physical size. Reads walk the PSRAM source row-wise
 * (a strip's pixels live in one logical column per destination row), writes
 * hit a small internal-RAM scratch, so no 300 KB staging buffer is needed.
 */
#define AXS_ROTATE_STRIP_ROWS 8

static uint16_t axs_rotate_scratch[AXS_ROTATE_STRIP_ROWS * AXS_WIDTH] __aligned(4);

/*
 * Full physical frame (300 KB) staged in PSRAM - the same placement and the
 * same GDMA path the LVGL draw buffer (`.lvgl_buf`) uses. The transpose
 * assembles the whole frame here and streams it as ONE RAMWR transaction,
 * byte-for-byte the protocol shape proven on the bench; a frame split into
 * many CS-asserted RAMWRC chunks is NOT proven on this panel (bench
 * 2026-11-22: 60-chunk streaming painted scattered pixel runs).
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

/* Full logical frame at the rotated shape? (The only flush shape LVGL's
 * full-refresh mode produces, and the only one the transpose can serve.) */
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

    /* Columns 0..AXS_WIDTH-1 inclusive - what the unrotated path programs for
     * every full-width flush. The CASET window sets the row wrap, so an
     * off-by-one here shifts every row by a pixel (diagonal shear; bench
     * 2026-11-22). */
    uint8_t caset[4] = {0x00, 0x00, (uint8_t)((AXS_WIDTH - 1) >> 8),
                        (uint8_t)((AXS_WIDTH - 1) & 0xFF)};

    err = axs15231b_tx_cmd(dev, AXS_CMD_CASET, caset, sizeof(caset));
    if (err) {
        return err;
    }

    /*
     * Transpose strip by strip (scratch keeps the read pattern cache-friendly
     * and the frame copies sequential) into the full physical frame, then
     * stream that as one frame - see axs_rotate_frame's note.
     */
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

    /*
     * Program the column window (CASET) - and only the column window. In QSPI
     * mode the AXS15231B's GRAM write is a plain row-pointer stream: the row
     * window (RASET) is ignored, RAMWR restarts the stream at row 0, and
     * RAMWRC ("continue writing") carries on from wherever the pointer sits
     * (the reference driver compiles RASET out of its QSPI path, and bench
     * 2026-11-21 confirmed rectangles sent with a RASET still land at the
     * pointer). So flushes must be full-width from row 0, which is the shape
     * LVGL produces with CONFIG_LV_Z_FULL_REFRESH; anything else writes
     * pixels to the wrong rows.
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

    if (y != 0) {
        LOG_WRN("AXS15231B: flush at y=%u cannot be placed (QSPI window is columns-only; "
                "run LVGL in full-refresh mode)", y);
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
    /*
     * RGB_565X = byte-swapped 565. The panel consumes 16-bit pixels
     * big-endian, like any MIPI DDI panel (datasheet's 16-bit figure puts
     * R4..R0 first); the flush path streams VDB bytes through unchanged,
     * so declaring the swapped format is what makes LVGL (mapped to
     * LV_COLOR_FORMAT_RGB565_SWAPPED by zephyr/modules/lvgl/lvgl_display.c)
     * emit MSB-first words at zero cost. Declaring plain RGB_565 instead
     * shows on the glass exactly as bench saw 2026-11-21: white text cores
     * crisp (0xFFFF/0x0000 are swap-invariant) but every anti-aliased grey
     * edge lands as coloured speckle, because a swapped grey is a
     * saturated colour. Every bring-up fill so far was 0x0000/0xFFFF,
     * which cannot tell the orders apart.
     */
    caps->supported_pixel_formats = PIXEL_FORMAT_RGB_565X;
    caps->current_pixel_format = PIXEL_FORMAT_RGB_565X;
    caps->screen_info = 0;
#if AXS_ROT_SWAPPED || (AXS_ROTATION_HW && AXS_ROTATION == 180)
    /* The LVGL pointer glue maps raw touch into these coordinates, so the
     * hardware route must not also rotate in LVGL (see main.c). */
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
    /* Orientation is fixed at init (from the node's `rotation`), and caps
     * report it; the LVGL pointer glue caches caps at its own init, so a
     * runtime flip could not be honoured end-to-end anyway. Accept the
     * configured value, refuse everything else. */
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
 * Put a real SPI mode 3 on the clock pad, the way the reference stack does.
 *
 * The panel is strapped for mode 3 - SCLK idles HIGH, data is captured on the
 * rising edge - and mspi_dev_config() sets the controller's mode register for
 * exactly that. Programmed through the core's ck_idle_edge/ck_out_edge pair
 * alone, though, the clock that reaches a GPIO-matrix pad is not mode 3: every
 * frame arrives one half-clock out of step. IDF's spi_master reaches mode 3 a
 * different way: it drives the clock signal through the GPIO matrix WITH THE
 * MATRIX INVERTER ENABLED (esp_rom_gpio_connect_out_signal(sclk, sig, invert,
 * ...)), and that is the waveform this panel accepts - measured on the bench
 * 2026-11-21, where only mode 0 direct and mode 3 inverted painted. So enable
 * the same inverter: the shield's pinctrl routes FSPICLK_OUT to this pad
 * un-inverted (Zephyr's ESP32 pinctrl has no notion of inversion), and this
 * flips the matrix inverter bit for it. The pad number must stay the pinctrl
 * clock group's pad in cobantab1a.overlay.
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
        /* Sleep-in first: gives the touch engine a second, deeper edge to
         * fall out of the latched mailbox state on. */
        (void)axs15231b_tx_cmd(dev, AXS_CMD_SLPIN, NULL, 0);
        k_msleep(AXS_SLPOUT_DELAY_MS);
    }

    /* send_init performs the reset itself (HW pin when wired, SWRESET
     * otherwise), wakes the panel, and replays the vendor table + DISPON. */
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
        /* Bytes always leave MSB first on this controller; the panel expects
         * that, and it is what mspi_esp32.c does whatever this field says. */
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
