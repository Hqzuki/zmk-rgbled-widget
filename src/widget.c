#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/led.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>

#include <zmk/battery.h>
#include <zmk/ble.h>
#include <zmk/endpoints.h>
#include <zmk/events/activity_state_changed.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/events/split_peripheral_status_changed.h>
#include <zmk/keymap.h>
#include <zmk/split/bluetooth/peripheral.h>

#include <zmk/split/central.h>

#include <zephyr/logging/log.h>

#include <zmk_rgbled_widget/widget.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define LED_GPIO_NODE_ID DT_COMPAT_GET_ANY_STATUS_OKAY(gpio_leds)

BUILD_ASSERT(DT_NODE_EXISTS(DT_ALIAS(led_red)),
             "An alias for a red LED is not found for RGBLED_WIDGET");
BUILD_ASSERT(DT_NODE_EXISTS(DT_ALIAS(led_green)),
             "An alias for a green LED is not found for RGBLED_WIDGET");
BUILD_ASSERT(DT_NODE_EXISTS(DT_ALIAS(led_blue)),
             "An alias for a blue LED is not found for RGBLED_WIDGET");

BUILD_ASSERT(!(SHOW_LAYER_CHANGE && SHOW_LAYER_COLORS),
             "CONFIG_RGBLED_WIDGET_SHOW_LAYER_CHANGE and CONFIG_RGBLED_WIDGET_SHOW_LAYER_COLORS "
             "are mutually exclusive");

// GPIO-based LED device and indices of red/green/blue LEDs inside its DT node
static const struct device *led_dev = DEVICE_DT_GET(LED_GPIO_NODE_ID);
static const uint8_t rgb_idx[] = {
    DT_NODE_CHILD_IDX(DT_ALIAS(led_red)),
    DT_NODE_CHILD_IDX(DT_ALIAS(led_green)),
    DT_NODE_CHILD_IDX(DT_ALIAS(led_blue)),
};

// map from color values to names, for logging
static const char *color_names[] = {"black", "red",     "green", "yellow",
                                    "blue",  "magenta", "cyan",  "white"};

#if SHOW_LAYER_COLORS
static const uint8_t layer_color_idx[] = {
    CONFIG_RGBLED_WIDGET_LAYER_0_COLOR,  CONFIG_RGBLED_WIDGET_LAYER_1_COLOR,
    CONFIG_RGBLED_WIDGET_LAYER_2_COLOR,  CONFIG_RGBLED_WIDGET_LAYER_3_COLOR,
    CONFIG_RGBLED_WIDGET_LAYER_4_COLOR,  CONFIG_RGBLED_WIDGET_LAYER_5_COLOR,
    CONFIG_RGBLED_WIDGET_LAYER_6_COLOR,  CONFIG_RGBLED_WIDGET_LAYER_7_COLOR,
    CONFIG_RGBLED_WIDGET_LAYER_8_COLOR,  CONFIG_RGBLED_WIDGET_LAYER_9_COLOR,
    CONFIG_RGBLED_WIDGET_LAYER_10_COLOR, CONFIG_RGBLED_WIDGET_LAYER_11_COLOR,
    CONFIG_RGBLED_WIDGET_LAYER_12_COLOR, CONFIG_RGBLED_WIDGET_LAYER_13_COLOR,
    CONFIG_RGBLED_WIDGET_LAYER_14_COLOR, CONFIG_RGBLED_WIDGET_LAYER_15_COLOR,
    CONFIG_RGBLED_WIDGET_LAYER_16_COLOR, CONFIG_RGBLED_WIDGET_LAYER_17_COLOR,
    CONFIG_RGBLED_WIDGET_LAYER_18_COLOR, CONFIG_RGBLED_WIDGET_LAYER_19_COLOR,
    CONFIG_RGBLED_WIDGET_LAYER_20_COLOR, CONFIG_RGBLED_WIDGET_LAYER_21_COLOR,
    CONFIG_RGBLED_WIDGET_LAYER_22_COLOR, CONFIG_RGBLED_WIDGET_LAYER_23_COLOR,
    CONFIG_RGBLED_WIDGET_LAYER_24_COLOR, CONFIG_RGBLED_WIDGET_LAYER_25_COLOR,
    CONFIG_RGBLED_WIDGET_LAYER_26_COLOR, CONFIG_RGBLED_WIDGET_LAYER_27_COLOR,
    CONFIG_RGBLED_WIDGET_LAYER_28_COLOR, CONFIG_RGBLED_WIDGET_LAYER_29_COLOR,
    CONFIG_RGBLED_WIDGET_LAYER_30_COLOR, CONFIG_RGBLED_WIDGET_LAYER_31_COLOR,
};
#endif

// log shorthands
#define LOG_CONN_CENTRAL(index, status, color_label)                                               \
    LOG_INF("Profile %d %s, blinking %s", index, status,                                           \
            color_names[CONFIG_RGBLED_WIDGET_CONN_COLOR_##color_label])
#define LOG_CONN_PERIPHERAL(status, color_label)                                                   \
    LOG_INF("Peripheral %s, blinking %s", status,                                                  \
            color_names[CONFIG_RGBLED_WIDGET_CONN_COLOR_##color_label])
#define LOG_BATTERY(battery_level, color_label)                                                    \
    LOG_INF("Battery level %d, blinking %s", battery_level,                                        \
            color_names[CONFIG_RGBLED_WIDGET_BATTERY_COLOR_##color_label])

// a blink work item as specified by the color and duration
struct blink_item {
    uint8_t color;
    uint16_t duration_ms;
    uint16_t sleep_ms;
};

// flag to indicate whether the initial boot up sequence is complete
static bool initialized = false;

// current physical LED output color
static uint8_t led_current_color = 0;

// persistent "base" color restored after temporary blinks
// central: layer color
// peripheral: battery color
static uint8_t led_persistent_color = 0;
static bool led_sleeping = false;

// low-level method to control the LED
static void set_rgb_leds(uint8_t color, uint16_t duration_ms) {
    for (uint8_t pos = 0; pos < 3; pos++) {
        uint8_t bit = BIT(pos);
        if ((bit & led_current_color) != (bit & color)) {
            if (bit & color) {
                led_on(led_dev, rgb_idx[pos]);
            } else {
                led_off(led_dev, rgb_idx[pos]);
            }
        }
    }
    if (duration_ms > 0) {
        k_sleep(K_MSEC(duration_ms));
    }
    led_current_color = color;
}

// define message queue of blink work items, that will be processed by a
// separate thread
K_MSGQ_DEFINE(led_msgq, sizeof(struct blink_item), 16, 1);

static void indicate_connectivity_internal(void) {
    struct blink_item blink = {.duration_ms = CONFIG_RGBLED_WIDGET_CONN_BLINK_MS};

#if !IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
#if IS_ENABLED(CONFIG_ZMK_BLE)
    uint8_t profile_index = zmk_ble_active_profile_index();
#endif

    switch (zmk_endpoints_selected().transport) {
    case ZMK_TRANSPORT_USB:
#if IS_ENABLED(CONFIG_RGBLED_WIDGET_CONN_SHOW_USB)
        LOG_INF("USB connected, blinking %s", color_names[CONFIG_RGBLED_WIDGET_CONN_COLOR_USB]);
        blink.color = CONFIG_RGBLED_WIDGET_CONN_COLOR_USB;
        break;
#endif
    case ZMK_TRANSPORT_BLE:
#if IS_ENABLED(CONFIG_ZMK_BLE)
        LOG_CONN_CENTRAL(profile_index, "connected", CONNECTED);
        blink.color = CONFIG_RGBLED_WIDGET_CONN_COLOR_CONNECTED;
        break;
#endif
    default:
#if IS_ENABLED(CONFIG_ZMK_BLE)
        if (zmk_ble_active_profile_is_open()) {
            LOG_CONN_CENTRAL(profile_index, "open", ADVERTISING);
            blink.color = CONFIG_RGBLED_WIDGET_CONN_COLOR_ADVERTISING;
            break;
        }
#endif
        LOG_CONN_CENTRAL(-1, "no endpoints connected", DISCONNECTED);
        blink.color = CONFIG_RGBLED_WIDGET_CONN_COLOR_DISCONNECTED;
        break;
    }
#elif IS_ENABLED(CONFIG_ZMK_SPLIT_BLE)
    if (zmk_split_bt_peripheral_is_connected()) {
        LOG_CONN_PERIPHERAL("connected", CONNECTED);
        blink.color = CONFIG_RGBLED_WIDGET_CONN_COLOR_CONNECTED;
    } else {
        LOG_CONN_PERIPHERAL("not connected", DISCONNECTED);
        blink.color = CONFIG_RGBLED_WIDGET_CONN_COLOR_DISCONNECTED;
    }
#endif

    k_msgq_put(&led_msgq, &blink, K_NO_WAIT);
}

static int led_output_listener_cb(const zmk_event_t *eh) {
    if (initialized) {
        indicate_connectivity();
    }
    return 0;
}

// debouncing to ignore all but last connectivity event, to prevent repeat blinks
static struct k_work_delayable indicate_connectivity_work;
static void indicate_connectivity_cb(struct k_work *work) { indicate_connectivity_internal(); }
void indicate_connectivity() { k_work_reschedule(&indicate_connectivity_work, K_MSEC(16)); }

ZMK_LISTENER(led_output_listener, led_output_listener_cb);

#if !IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
#if IS_ENABLED(CONFIG_RGBLED_WIDGET_CONN_SHOW_USB)
ZMK_SUBSCRIPTION(led_output_listener, zmk_endpoint_changed);
#endif
#if IS_ENABLED(CONFIG_ZMK_BLE)
ZMK_SUBSCRIPTION(led_output_listener, zmk_ble_active_profile_changed);
#endif
#elif IS_ENABLED(CONFIG_ZMK_SPLIT_BLE)
ZMK_SUBSCRIPTION(led_output_listener, zmk_split_peripheral_status_changed);
#endif

#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)
static inline uint8_t get_battery_color(uint8_t battery_level) {
    if (battery_level == 0) {
        LOG_INF("Battery level undetermined (zero), blinking %s",
                color_names[CONFIG_RGBLED_WIDGET_BATTERY_COLOR_MISSING]);
        return CONFIG_RGBLED_WIDGET_BATTERY_COLOR_MISSING;
    }
    if (battery_level >= CONFIG_RGBLED_WIDGET_BATTERY_LEVEL_HIGH) {
        LOG_BATTERY(battery_level, HIGH);
        return CONFIG_RGBLED_WIDGET_BATTERY_COLOR_HIGH;
    }
    if (battery_level >= CONFIG_RGBLED_WIDGET_BATTERY_LEVEL_LOW) {
        LOG_BATTERY(battery_level, MEDIUM);
        return CONFIG_RGBLED_WIDGET_BATTERY_COLOR_MEDIUM;
    }
    LOG_BATTERY(battery_level, LOW);
    return CONFIG_RGBLED_WIDGET_BATTERY_COLOR_LOW;
}

void indicate_battery(void) {
    struct blink_item blink = {.duration_ms = CONFIG_RGBLED_WIDGET_BATTERY_BLINK_MS};
    int retry = 0;

#if IS_ENABLED(CONFIG_RGBLED_WIDGET_BATTERY_SHOW_SELF) ||                                          \
    IS_ENABLED(CONFIG_RGBLED_WIDGET_BATTERY_SHOW_PERIPHERALS)
    uint8_t battery_level = zmk_battery_state_of_charge();
    while (battery_level == 0 && retry++ < 10) {
        k_sleep(K_MSEC(100));
        battery_level = zmk_battery_state_of_charge();
    };

    blink.color = get_battery_color(battery_level);
    k_msgq_put(&led_msgq, &blink, K_NO_WAIT);
#endif

#if IS_ENABLED(CONFIG_RGBLED_WIDGET_BATTERY_SHOW_PERIPHERALS) ||                                   \
    IS_ENABLED(CONFIG_RGBLED_WIDGET_BATTERY_SHOW_ONLY_PERIPHERALS)
    for (uint8_t i = 0; i < ZMK_SPLIT_BLE_PERIPHERAL_COUNT; i++) {
        uint8_t peripheral_level;
        int ret = zmk_split_central_get_peripheral_battery_level(i, &peripheral_level);
        if (ret == 0) {
            retry = 0;
            while (peripheral_level == 0 &&
                   retry++ < (CONFIG_RGBLED_WIDGET_BATTERY_BLINK_MS +
                              CONFIG_RGBLED_WIDGET_INTERVAL_MS) / 100) {
                k_sleep(K_MSEC(100));
                zmk_split_central_get_peripheral_battery_level(i, &peripheral_level);
            }

            LOG_INF("Got battery level for peripheral %d:", i);
            blink.color = get_battery_color(peripheral_level);
            k_msgq_put(&led_msgq, &blink, K_NO_WAIT);
        } else {
            LOG_ERR("Error looking up battery level for peripheral %d", i);
        }
    }
#endif
}
#endif // IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)

#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING) && IS_ENABLED(CONFIG_ZMK_SPLIT) &&                    \
    !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
static struct k_work_delayable battery_critical_blink_work;

static void sync_battery_critical_blink(uint8_t battery_level) {
    if (!initialized || led_sleeping || battery_level == 0 ||
        battery_level > CONFIG_RGBLED_WIDGET_BATTERY_LEVEL_CRITICAL) {
        k_work_cancel_delayable(&battery_critical_blink_work);
        return;
    }

    k_work_reschedule(&battery_critical_blink_work, K_NO_WAIT);
}

static void battery_critical_blink_cb(struct k_work *work) {
    ARG_UNUSED(work);

    uint8_t battery_level = zmk_battery_state_of_charge();
    if (!initialized || led_sleeping || battery_level == 0 ||
        battery_level > CONFIG_RGBLED_WIDGET_BATTERY_LEVEL_CRITICAL) {
        return;
    }

    struct blink_item blink = {
        .duration_ms = CONFIG_RGBLED_WIDGET_BATTERY_BLINK_MS,
        .color = CONFIG_RGBLED_WIDGET_BATTERY_COLOR_CRITICAL,
    };
    k_msgq_put(&led_msgq, &blink, K_NO_WAIT);
    k_work_reschedule(&battery_critical_blink_work,
                      K_MSEC(CONFIG_RGBLED_WIDGET_BATTERY_BLINK_MS +
                             CONFIG_RGBLED_WIDGET_INTERVAL_MS));
}

static void update_battery_persistent_color(void) {
    uint8_t battery_level = zmk_battery_state_of_charge();
    uint8_t color = get_battery_color(battery_level);

    if (led_persistent_color != color) {
        led_persistent_color = color;
        struct blink_item item = {.color = led_persistent_color};
        k_msgq_put(&led_msgq, &item, K_NO_WAIT);
    }

    sync_battery_critical_blink(battery_level);
}
#endif

#if SHOW_LAYER_COLORS && (!IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL))
static void update_layer_color(void) {
    uint8_t index = zmk_keymap_highest_layer_active();

    if (led_persistent_color != layer_color_idx[index]) {
        led_persistent_color = layer_color_idx[index];
        struct blink_item color = {.color = led_persistent_color};
        LOG_INF("Setting layer color to %s for layer %d", color_names[led_persistent_color], index);
        k_msgq_put(&led_msgq, &color, K_NO_WAIT);
    }
}
#endif

static void update_persistent_color(void) {
#if SHOW_LAYER_COLORS && (!IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL))
    update_layer_color();
#elif IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING) && IS_ENABLED(CONFIG_ZMK_SPLIT) &&                  \
    !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    update_battery_persistent_color();
#endif
}

#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)
static int led_battery_listener_cb(const zmk_event_t *eh) {
    if (!initialized) {
        return 0;
    }

#if IS_ENABLED(CONFIG_ZMK_SPLIT) && !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    update_battery_persistent_color();
    return 0;
#endif

    uint8_t battery_level = as_zmk_battery_state_changed(eh)->state_of_charge;

    if (battery_level > 0 && battery_level <= CONFIG_RGBLED_WIDGET_BATTERY_LEVEL_CRITICAL) {
        LOG_BATTERY(battery_level, CRITICAL);

        struct blink_item blink = {
            .duration_ms = CONFIG_RGBLED_WIDGET_BATTERY_BLINK_MS,
            .color = CONFIG_RGBLED_WIDGET_BATTERY_COLOR_CRITICAL,
        };
        k_msgq_put(&led_msgq, &blink, K_NO_WAIT);
    }
    return 0;
}

ZMK_LISTENER(led_battery_listener, led_battery_listener_cb);
ZMK_SUBSCRIPTION(led_battery_listener, zmk_battery_state_changed);
#endif

static int led_persistent_listener_cb(const zmk_event_t *eh) {
    struct zmk_activity_state_changed *ev = as_zmk_activity_state_changed(eh);

    if (ev != NULL) {
        switch (ev->state) {
        case ZMK_ACTIVITY_SLEEP:
            LOG_INF("Detected sleep activity state, turn off LED");
            led_sleeping = true;
#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING) && IS_ENABLED(CONFIG_ZMK_SPLIT) &&                    \
    !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
            k_work_cancel_delayable(&battery_critical_blink_work);
#endif
            set_rgb_leds(0, 0);
            break;
        case ZMK_ACTIVITY_ACTIVE:
        case ZMK_ACTIVITY_IDLE:
            led_sleeping = false;
            if (initialized) {
                update_persistent_color();
            }
            break;
        default:
            break;
        }
        return 0;
    }

    if (initialized) {
        update_persistent_color();
    }
    return 0;
}

ZMK_LISTENER(led_persistent_listener, led_persistent_listener_cb);
ZMK_SUBSCRIPTION(led_persistent_listener, zmk_activity_state_changed);

#if SHOW_LAYER_COLORS && (!IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL))
ZMK_SUBSCRIPTION(led_persistent_listener, zmk_layer_state_changed);
#endif

#if !IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
void indicate_layer(void) {
    uint8_t index = zmk_keymap_highest_layer_active();
    static const struct blink_item blink = {
        .duration_ms = CONFIG_RGBLED_WIDGET_LAYER_BLINK_MS,
        .color = CONFIG_RGBLED_WIDGET_LAYER_COLOR,
        .sleep_ms = CONFIG_RGBLED_WIDGET_LAYER_BLINK_MS,
    };
    static const struct blink_item last_blink = {
        .duration_ms = CONFIG_RGBLED_WIDGET_LAYER_BLINK_MS,
        .color = CONFIG_RGBLED_WIDGET_LAYER_COLOR,
    };

    LOG_INF("Blinking %d times %s for layer change", index,
            color_names[CONFIG_RGBLED_WIDGET_LAYER_COLOR]);

    for (int i = 0; i < index; i++) {
        if (i < index - 1) {
            k_msgq_put(&led_msgq, &blink, K_NO_WAIT);
        } else {
            k_msgq_put(&led_msgq, &last_blink, K_NO_WAIT);
        }
    }
}
#endif

#if SHOW_LAYER_CHANGE
static struct k_work_delayable layer_indicate_work;

static int led_layer_listener_cb(const zmk_event_t *eh) {
    if (initialized && as_zmk_layer_state_changed(eh)->state) {
        k_work_reschedule(&layer_indicate_work, K_MSEC(CONFIG_RGBLED_WIDGET_LAYER_DEBOUNCE_MS));
    }
    return 0;
}

static void indicate_layer_cb(struct k_work *work) { indicate_layer(); }

ZMK_LISTENER(led_layer_listener, led_layer_listener_cb);
ZMK_SUBSCRIPTION(led_layer_listener, zmk_layer_state_changed);
#endif

extern void led_process_thread(void *d0, void *d1, void *d2) {
    ARG_UNUSED(d0);
    ARG_UNUSED(d1);
    ARG_UNUSED(d2);

    k_work_init_delayable(&indicate_connectivity_work, indicate_connectivity_cb);

#if SHOW_LAYER_CHANGE
    k_work_init_delayable(&layer_indicate_work, indicate_layer_cb);
#endif
#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING) && IS_ENABLED(CONFIG_ZMK_SPLIT) &&                    \
    !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    k_work_init_delayable(&battery_critical_blink_work, battery_critical_blink_cb);
#endif

    while (true) {
        struct blink_item blink;
        k_msgq_get(&led_msgq, &blink, K_FOREVER);

        if (blink.duration_ms > 0) {
            LOG_DBG("Got a blink item from msgq, color %d, duration %d", blink.color,
                    blink.duration_ms);

            if (blink.color == led_current_color && blink.color > 0) {
                set_rgb_leds(0, CONFIG_RGBLED_WIDGET_INTERVAL_MS);
            }

            set_rgb_leds(blink.color, blink.duration_ms);

            if (blink.color == led_persistent_color && blink.color > 0) {
                set_rgb_leds(0, CONFIG_RGBLED_WIDGET_INTERVAL_MS);
            }

            set_rgb_leds(led_persistent_color,
                         blink.sleep_ms > 0 ? blink.sleep_ms
                                            : CONFIG_RGBLED_WIDGET_INTERVAL_MS);
        } else {
            LOG_DBG("Got a persistent color item from msgq, color %d", blink.color);
            set_rgb_leds(blink.color, 0);
        }
    }
}

K_THREAD_DEFINE(led_process_tid, 1024, led_process_thread, NULL, NULL, NULL,
                K_LOWEST_APPLICATION_THREAD_PRIO, 0, 100);

extern void led_init_thread(void *d0, void *d1, void *d2) {
    ARG_UNUSED(d0);
    ARG_UNUSED(d1);
    ARG_UNUSED(d2);

#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)
    LOG_INF("Indicating initial battery status");
    indicate_battery();
    k_sleep(K_MSEC(CONFIG_RGBLED_WIDGET_BATTERY_BLINK_MS + CONFIG_RGBLED_WIDGET_INTERVAL_MS));
#endif

    LOG_INF("Indicating initial connectivity status");
    indicate_connectivity();

    LOG_INF("Setting initial persistent color");
    update_persistent_color();

    initialized = true;
    LOG_INF("Finished initializing LED widget");
}

K_THREAD_DEFINE(led_init_tid, 1024, led_init_thread, NULL, NULL, NULL,
                K_LOWEST_APPLICATION_THREAD_PRIO, 0, 200);
