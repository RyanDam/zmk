#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/led.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>

#include <zmk/battery.h>
#include <zmk/ble.h>
#include <zmk/endpoints.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/keymap.h>

#include <zephyr/logging/log.h>

#include <indicator/indicator.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define LED_GPIO_NODE_ID DT_COMPAT_GET_ANY_STATUS_OKAY(gpio_leds)

BUILD_ASSERT(DT_NODE_EXISTS(DT_ALIAS(led_l0)),
             "An alias for 1st LED is not found");
BUILD_ASSERT(DT_NODE_EXISTS(DT_ALIAS(led_l1)),
             "An alias for 2nd green LED is not found");
BUILD_ASSERT(DT_NODE_EXISTS(DT_ALIAS(led_l2)),
             "An alias for 3rd blue LED is not found");
BUILD_ASSERT(DT_NODE_EXISTS(DT_ALIAS(led_l3)),
             "An alias for 4th blue LED is not found");

// GPIO-based LED device and indices of red/green/blue LEDs inside its DT node
static const struct device *led_dev = DEVICE_DT_GET(LED_GPIO_NODE_ID);
static const uint8_t led_idx[] = {DT_NODE_CHILD_IDX(DT_ALIAS(led_l0)),
                                  DT_NODE_CHILD_IDX(DT_ALIAS(led_l1)),
                                  DT_NODE_CHILD_IDX(DT_ALIAS(led_l2)),
                                  DT_NODE_CHILD_IDX(DT_ALIAS(led_l3))};

// map from color values to names, for logging
static const uint8_t COLOR_BLACK = 0;
static const uint8_t COLOR_IND_1 = 1 << 0;
static const uint8_t COLOR_IND_2 = 1 << 1;
static const uint8_t COLOR_IND_3 = 1 << 2;
static const uint8_t COLOR_IND_4 = 1 << 3;
static const uint8_t COLOR_WHITE = COLOR_IND_1 | COLOR_IND_2 | COLOR_IND_3 | COLOR_IND_4;

static const uint8_t color_idx[] = {
    COLOR_BLACK, COLOR_IND_1, COLOR_IND_2, COLOR_IND_3, COLOR_IND_4, COLOR_WHITE,
    COLOR_IND_1 | COLOR_IND_2, COLOR_IND_1 | COLOR_IND_2 | COLOR_IND_3
};
static const char *color_names[] = {
    "----", "x---",  "-x--", "--x-", "---x", "xxxx",
    "xx--", "xxx-"
};

// log shorthands
#define LOG_CONN_CENTRAL(index, status)                                                \
    LOG_INF("Profile %d %s", index, status)                               
#define LOG_CONN_PERIPHERAL(status, color_label)                                       \
    LOG_INF("Peripheral %s, blinking %s", status, color_names[color_label])
#define LOG_BATTERY(battery_level, color_label)                                        \
    LOG_INF("Battery level %d, blinking %s", battery_level, color_names[color_label])
#define LOG_LAYER(layer_index, color_label)                                            \
    LOG_INF("Layer %d, blinking %s", layer_index, color_names[color_label])

// a blink work item as specified by the color and duration
struct blink_item {
    uint8_t color;
    uint8_t blink_time;
    uint16_t duration_ms;
    uint16_t sleep_ms;
};

// flag to indicate whether the initial boot up sequence is complete
static bool initialized = false;

// define message queue of blink work items, that will be processed by a
// separate thread
K_MSGQ_DEFINE(led_msgq, sizeof(struct blink_item), 16, 1);

#if IS_ENABLED(CONFIG_ZMK_BLE)

void indicate_connectivity(void) {
    struct blink_item blink = {.duration_ms = 500, .sleep_ms = 200, .blink_time = 10};

    uint8_t profile_index = zmk_ble_active_profile_index();

    switch (profile_index) {
        case 0:
            blink.color = COLOR_IND_1;
            break;
        case 1:
            blink.color = COLOR_IND_2;
            break;
        case 2:
            blink.color = COLOR_IND_3;
            break;
        case 3:
            blink.color = COLOR_IND_4;
            break;
        default:
            blink.color = COLOR_WHITE;
    }

    if (zmk_ble_active_profile_is_connected()) {
        LOG_CONN_CENTRAL(profile_index, "connected");
        blink.duration_ms = 2000;
        blink.blink_time = 0;
    } else if (zmk_ble_active_profile_is_open()) {
        LOG_CONN_CENTRAL(profile_index, "open");
        blink.duration_ms = 200;
    } else {
        LOG_CONN_CENTRAL(profile_index, "not connected");
        blink.duration_ms = 500;
    }

    k_msgq_put(&led_msgq, &blink, K_NO_WAIT);
}

static int led_output_listener_cb(const zmk_event_t *eh) {
    if (initialized) {
        indicate_connectivity();
    }
    return 0;
}

ZMK_LISTENER(led_output_listener, led_output_listener_cb);
ZMK_SUBSCRIPTION(led_output_listener, zmk_ble_active_profile_changed);

#endif // IS_ENABLED(CONFIG_ZMK_BLE)

#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)

void indicate_battery(void) {
    struct blink_item blink = {.duration_ms = 500};
    uint8_t battery_level = zmk_battery_state_of_charge();
    int retry = 0;
    while (battery_level == 0 && retry++ < 10) {
        k_sleep(K_MSEC(100));
        battery_level = zmk_battery_state_of_charge();
    };

    if (battery_level == 0) {
        LOG_INF("Battery level undetermined (zero), blinking magenta");
        blink.color = COLOR_IND_1;
    } else if (battery_level >= 80) {
        blink.color = COLOR_IND_1 | COLOR_IND_2 | COLOR_IND_3 | COLOR_IND_4;
        LOG_BATTERY(battery_level, 5);
    } else if (battery_level >= 50) {
        LOG_BATTERY(battery_level, 7);
        blink.color = COLOR_IND_1 | COLOR_IND_2 | COLOR_IND_3;
    } else {
        LOG_BATTERY(battery_level, 6);
        blink.color = COLOR_IND_1 | COLOR_IND_2;
    }

    k_msgq_put(&led_msgq, &blink, K_NO_WAIT);
}

static int led_battery_listener_cb(const zmk_event_t *eh) {
    if (!initialized) {
        return 0;
    }
    // check if we are in critical battery levels at state change, blink if we are
    uint8_t battery_level = as_zmk_battery_state_changed(eh)->state_of_charge;
    if (battery_level > 0 && battery_level <= 5) {
        struct blink_item blink = {.duration_ms = 500, .color = COLOR_WHITE, .blink_time = 5, .sleep_ms = 500};
        k_msgq_put(&led_msgq, &blink, K_NO_WAIT);
    }
    return 0;
}

// run led_battery_listener_cb on battery state change event
ZMK_LISTENER(led_battery_listener, led_battery_listener_cb);
ZMK_SUBSCRIPTION(led_battery_listener, zmk_battery_state_changed);
#endif // IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)
 
void indicate_layer(void) {
    uint8_t index = zmk_keymap_highest_layer_active();
    struct blink_item blink = {.duration_ms = 500, .sleep_ms = 100};
    LOG_LAYER(index, index);
    blink.color = color_idx[index+1];
    k_msgq_put(&led_msgq, &blink, K_NO_WAIT);
}

static int led_layer_listener_cb(const zmk_event_t *eh) {
    // ignore if not initialized yet or layer off events
    if (initialized) {
        if (as_zmk_layer_state_changed(eh)->state && as_zmk_layer_state_changed(eh)->layer > 0) {
            // go to other not default layer
            indicate_layer();
        } else if (as_zmk_layer_state_changed(eh)->layer == 3 || as_zmk_layer_state_changed(eh)->layer == 4) {
            // go to default layer
            indicate_layer();
        }
    }
    return 0;
}   

ZMK_LISTENER(led_layer_listener, led_layer_listener_cb);
ZMK_SUBSCRIPTION(led_layer_listener, zmk_layer_state_changed);

#define BLINK_STATE_IDLE 0
#define BLINK_STATE_ON 1
#define BLINK_STATE_OFF 2

extern void led_process_thread(void *d0, void *d1, void *d2) {
    ARG_UNUSED(d0);
    ARG_UNUSED(d1);
    ARG_UNUSED(d2);

    struct blink_item blink;
    uint32_t start_time = 0;
    int state = BLINK_STATE_IDLE;
    uint8_t blink_left = 0;
    bool state_changed = false;

    while (true) {

        if (k_msgq_get(&led_msgq, &blink, state == BLINK_STATE_IDLE ? K_FOREVER : K_NO_WAIT) == 0) {
            // If new blink item is received, reset the state and start time
            LOG_DBG("Got a blink item from msgq, color %d, duration %d",
                    blink.color, blink.duration_ms);
            start_time = k_uptime_get();
            state = BLINK_STATE_ON;
            blink_left = blink.blink_time > 0 ? blink.blink_time : 0;
            state_changed = true;
        }

        // BLINK life cycle: ( ON -> OFF ) ^ blink_left -> IDLE
        if (state == BLINK_STATE_ON) {
            // check if the blink duration has elapsed
            if ((k_uptime_get() - start_time) >= blink.duration_ms) {
                state = BLINK_STATE_OFF;
                state_changed = true;
                start_time = k_uptime_get();
            }
        } else if (state == BLINK_STATE_OFF) {
            // check if the blink duration has elapsed
            if ((k_uptime_get() - start_time) >= blink.sleep_ms) {
                if (blink_left > 0) {
                    blink_left--;
                    state = BLINK_STATE_ON;
                } else { 
                    state = BLINK_STATE_IDLE;
                }
                state_changed = true;
                start_time = k_uptime_get();
            }
        }

        // update LEDs only when state changes
        if (state_changed) {
            if (state == BLINK_STATE_ON) {
                // turn appropriate LEDs on
                if (blink.color & COLOR_IND_1) led_on(led_dev, led_idx[0]);
                else led_off(led_dev, led_idx[0]);
                if (blink.color & COLOR_IND_2) led_on(led_dev, led_idx[1]);
                else led_off(led_dev, led_idx[1]);
                if (blink.color & COLOR_IND_3) led_on(led_dev, led_idx[2]);
                else led_off(led_dev, led_idx[2]);
                if (blink.color & COLOR_IND_4) led_on(led_dev, led_idx[3]);
                else led_off(led_dev, led_idx[3]);
            } else if (state == BLINK_STATE_OFF) {
                // turn appropriate LEDs off
                for (uint8_t pos = 0; pos < 4; pos++) {
                    led_off(led_dev, led_idx[pos]);
                }
            }
            state_changed = false;
        }
        
        // sleep for 10 ms before checking again
        k_sleep(K_MSEC(10));
    }
}

// define led_process_thread with stack size 1024, start running it 100 ms after
// boot
K_THREAD_DEFINE(led_process_tid, 1024, led_process_thread, NULL, NULL, NULL,
                K_LOWEST_APPLICATION_THREAD_PRIO, 0, 100);

extern void led_init_thread(void *d0, void *d1, void *d2) {
    ARG_UNUSED(d0);
    ARG_UNUSED(d1);
    ARG_UNUSED(d2);

// #if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)
//     // check and indicate battery level on thread start
//     LOG_INF("Indicating initial battery status");
//     indicate_battery();
// #endif // IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)

#if IS_ENABLED(CONFIG_ZMK_BLE)
    // check and indicate current profile or peripheral connectivity status
    LOG_INF("Indicating initial connectivity status");
    indicate_connectivity();
#endif // IS_ENABLED(CONFIG_ZMK_BLE)

    initialized = true;
    LOG_INF("Finished led_init_thread");
}

// run init thread on boot for initial battery+output checks
K_THREAD_DEFINE(led_init_tid, 1024, led_init_thread, NULL, NULL, NULL,
                K_LOWEST_APPLICATION_THREAD_PRIO, 0, 200);
