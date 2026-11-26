/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/ring_buffer.h>

#include <zephyr/logging/log.h>
#include <zmk/studio/rpc.h>
#include <zmk/event_manager.h>
#include <zmk/events/usb_conn_state_changed.h>

LOG_MODULE_DECLARE(zmk_studio, CONFIG_ZMK_STUDIO_LOG_LEVEL);

/* change this to any other UART peripheral if desired */
#define UART_DEVICE_NODE DT_CHOSEN(zmk_studio_rpc_uart)

static const struct device *const uart_dev = DEVICE_DT_GET(UART_DEVICE_NODE);

static void tx_notify(struct ring_buf *tx_ring_buf, size_t written, bool msg_done,
                      void *user_data) {
    if (msg_done || (ring_buf_size_get(tx_ring_buf) > (ring_buf_capacity_get(tx_ring_buf) / 2))) {
#if IS_ENABLED(CONFIG_UART_INTERRUPT_DRIVEN)
        LOG_DBG("UART RPC transport: tx_notify -> uart_irq_tx_enable (msg_done=%d, tx_buf=%u)",
                msg_done, ring_buf_size_get(tx_ring_buf));
        uart_irq_tx_enable(uart_dev);
#else
        struct ring_buf *tx_buf = zmk_rpc_get_tx_buf();
        uint8_t *buf;
        uint32_t chunk_len;
        while ((chunk_len = ring_buf_get_ptr(tx_buf, &buf, 0)) > 0) {
            for (uint32_t i = 0; i < chunk_len; i++) {
                uart_poll_out(uart_dev, buf[i]);
            }

            ring_buf_consume(tx_buf, chunk_len);
        }
#endif
    }
}

#if !IS_ENABLED(CONFIG_UART_INTERRUPT_DRIVEN)

static void uart_rx_main(void) {
    for (;;) {
        uint8_t *buf;
        struct ring_buf *ring_buf = zmk_rpc_get_rx_buf();
        uint32_t room = ring_buf_put_ptr(ring_buf, &buf, 0);

        if (room < 1) {
            LOG_WRN("No room in the RPC RX ring");
            k_sleep(K_MSEC(1));
            continue;
        }

        if (uart_poll_in(uart_dev, buf) < 0) {
            /* Nothing was written, so nothing is committed: the slot stays
             * free for the next pass. */
            k_sleep(K_MSEC(1));
        } else {
            ring_buf_commit(ring_buf, 1);
            zmk_rpc_rx_notify();
        }
    }
}

K_THREAD_DEFINE(uart_transport_read_thread, CONFIG_ZMK_STUDIO_TRANSPORT_UART_RX_STACK_SIZE,
                uart_rx_main, NULL, NULL, NULL, CONFIG_ZMK_STUDIO_TRANSPORT_UART_RX_PRIORITY, 0, 0);

#endif

static int start_rx() {
#if IS_ENABLED(CONFIG_UART_INTERRUPT_DRIVEN)
    LOG_INF("UART RPC transport: starting RX on %s", uart_dev->name);
    uart_irq_rx_enable(uart_dev);
#else
    k_thread_resume(uart_transport_read_thread);
#endif
    return 0;
}

static int stop_rx(void) {
#if IS_ENABLED(CONFIG_UART_INTERRUPT_DRIVEN)
    LOG_INF("UART RPC transport: stopping RX on %s", uart_dev->name);
    uart_irq_rx_disable(uart_dev);
#else
    k_thread_suspend(uart_transport_read_thread);
#endif
    return 0;
}

ZMK_RPC_TRANSPORT(uart, ZMK_TRANSPORT_USB, start_rx, stop_rx, NULL, tx_notify);

#if IS_ENABLED(CONFIG_UART_INTERRUPT_DRIVEN)

/*
 * Read characters from UART until line end is detected. Afterwards push the
 * data to the message queue.
 */
static void serial_cb(const struct device *dev, void *user_data) {
    /* uart_irq_update() returns void as of Zephyr 4.5: nothing to check. */
    uart_irq_update(uart_dev);

    if (uart_irq_rx_ready(uart_dev)) {
        /* read until FIFO empty */
        uint32_t last_read = 0, len = 0, total_read = 0;
        struct ring_buf *buf = zmk_rpc_get_rx_buf();
        do {
            uint8_t *buffer;
            len = ring_buf_put_ptr(buf, &buffer, 0);
            if (len > 0) {
                last_read = uart_fifo_read(uart_dev, buffer, len);

                ring_buf_commit(buf, last_read);
                total_read += last_read;
            } else {
                LOG_ERR("Dropping incoming RPC byte, insufficient room in the RX buffer. Bump "
                        "CONFIG_ZMK_STUDIO_RPC_RX_BUF_SIZE.");
                uint8_t dummy;
                last_read = uart_fifo_read(uart_dev, &dummy, 1);
            }
        } while (last_read && last_read == len);

        LOG_DBG("UART RPC transport: RX %u byte(s) on %s", total_read, uart_dev->name);
        zmk_rpc_rx_notify();
    }

    if (uart_irq_tx_ready(uart_dev)) {
        struct ring_buf *tx_buf = zmk_rpc_get_tx_buf();
        uint32_t len;
        uint32_t total_sent = 0;
        while ((len = ring_buf_size_get(tx_buf)) > 0) {
            uint8_t *buf;
            uint32_t avail = ring_buf_get_ptr(tx_buf, &buf, 0);

            if (avail == 0) {
                continue;
            }

            int sent = uart_fifo_fill(uart_dev, buf, avail);
            total_sent += MAX(sent, 0);

            ring_buf_consume(tx_buf, MAX(sent, 0));
        }

        LOG_DBG("UART RPC transport: TX %u byte(s) on %s", total_sent, uart_dev->name);
    }
}

#endif

static int uart_rpc_interface_init(void) {
    if (!device_is_ready(uart_dev)) {
        LOG_ERR("UART device not found!");
        return -ENODEV;
    }

    LOG_INF("UART RPC transport: using device %s", uart_dev->name);

#if IS_ENABLED(CONFIG_UART_INTERRUPT_DRIVEN)
    /* configure interrupt and callback to receive data */
    int ret = uart_irq_callback_user_data_set(uart_dev, serial_cb, NULL);

    if (ret < 0) {
        if (ret == -ENOTSUP) {
            printk("Interrupt-driven UART API support not enabled\n");
        } else if (ret == -ENOSYS) {
            printk("UART device does not support interrupt-driven API\n");
        } else {
            printk("Error setting UART callback: %d\n", ret);
        }
        return ret;
    }
#endif // IS_ENABLED(CONFIG_UART_INTERRUPT_DRIVEN)

    return 0;
}

SYS_INIT(uart_rpc_interface_init, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);

#if IS_ENABLED(CONFIG_UART_INTERRUPT_DRIVEN) && IS_ENABLED(CONFIG_ZMK_USB_DEVICE)

/* CDC ACM never re-enqueues its bulk-OUT RX buffer after suspend/resume, silently
 * stopping RX. Re-arm RX on every USB-ready event; uart_irq_rx_enable() is idempotent. */
static int uart_rpc_listener(const zmk_event_t *eh) {
    struct zmk_usb_conn_state_changed *usb_changed = as_zmk_usb_conn_state_changed(eh);
    if (usb_changed && usb_changed->conn_state == ZMK_USB_CONN_HID) {
        LOG_DBG("UART RPC transport: USB ready, re-arming RX on %s", uart_dev->name);
        uart_irq_rx_enable(uart_dev);
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(uart_rpc, uart_rpc_listener);
ZMK_SUBSCRIPTION(uart_rpc, zmk_usb_conn_state_changed);

#endif /* CONFIG_UART_INTERRUPT_DRIVEN && CONFIG_ZMK_USB_DEVICE */
