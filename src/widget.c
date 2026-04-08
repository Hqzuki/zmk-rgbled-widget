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
#include <zmk/events/endpoint_changed.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/events/split_peripheral_status_changed.h>
#include <zmk/events/activity_state_changed.h>
#include <zmk/keymap.h>
#include <zmk/split/bluetooth/peripheral.h>

#if __has_include(<zmk/split/central.h>)
#include <zmk/split/central.h>
#else
#include <zmk/split/bluetooth/central.h>
#endif

#include <zephyr/logging/log.h>
#include <zmk_rgbled_widget/widget.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define LED_GPIO_NODE_ID DT_COMPAT_GET_ANY_STATUS_OKAY(gpio_leds)

BUILD_ASSERT(DT_NODE_EXISTS(DT_ALIAS(led_red)), "Missing led_red");
BUILD_ASSERT(DT_NODE_EXISTS(DT_ALIAS(led_green)), "Missing led_green");
BUILD_ASSERT(DT_NODE_EXISTS(DT_ALIAS(led_blue)), "Missing led_blue");

static const struct device *led_dev = DEVICE_DT_GET(LED_GPIO_NODE_ID);
static const uint8_t rgb_idx[] = {
DT_NODE_CHILD_IDX(DT_ALIAS(led_red)),
DT_NODE_CHILD_IDX(DT_ALIAS(led_green)),
DT_NODE_CHILD_IDX(DT_ALIAS(led_blue))
};

static const char *color_names[] = {
"black","red","green","yellow","blue","magenta","cyan","white"
};

#if SHOW_LAYER_COLORS
static const uint8_t layer_color_idx[] = {
CONFIG_RGBLED_WIDGET_LAYER_0_COLOR,
CONFIG_RGBLED_WIDGET_LAYER_1_COLOR,
CONFIG_RGBLED_WIDGET_LAYER_2_COLOR,
CONFIG_RGBLED_WIDGET_LAYER_3_COLOR
};
#endif

struct blink_item {
uint8_t color;
uint16_t duration_ms;
uint16_t sleep_ms;
};

static bool initialized = false;
uint8_t led_current_color = 0;

/* ★追加：常時表示色 */
uint8_t led_persistent_color = 0;

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

K_MSGQ_DEFINE(led_msgq, sizeof(struct blink_item), 16, 1);

/* ===== battery ===== */

#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)
static inline uint8_t get_battery_color(uint8_t level) {
if (level >= 70) return 2; // green
if (level >= 30) return 3; // yellow
return 1; // red
}
#endif

#if IS_ENABLED(CONFIG_ZMK_SPLIT) && !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
static void update_battery_persistent_color(void) {
uint8_t level = zmk_battery_state_of_charge();
if (level == 0) return;

```
uint8_t color = get_battery_color(level);

if (led_persistent_color != color) {
    led_persistent_color = color;
    struct blink_item item = {.color = color};
    k_msgq_put(&led_msgq, &item, K_NO_WAIT);
}
```

}
#endif

/* ===== layer ===== */

uint8_t led_layer_color = 0;

#if SHOW_LAYER_COLORS
void update_layer_color(void) {
uint8_t index = zmk_keymap_highest_layer_active();

```
if (led_layer_color != layer_color_idx[index]) {
    led_layer_color = layer_color_idx[index];
    led_persistent_color = led_layer_color; // ★ここ重要
    struct blink_item item = {.color = led_layer_color};
    k_msgq_put(&led_msgq, &item, K_NO_WAIT);
}
```

}
#endif

/* ===== battery listener ===== */

#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)
static int led_battery_listener_cb(const zmk_event_t *eh) {
if (!initialized) return 0;

#if IS_ENABLED(CONFIG_ZMK_SPLIT) && !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
update_battery_persistent_color();
#endif

```
return 0;
```

}

ZMK_LISTENER(led_battery_listener, led_battery_listener_cb);
ZMK_SUBSCRIPTION(led_battery_listener, zmk_battery_state_changed);
#endif

/* ===== layer listener ===== */

#if SHOW_LAYER_COLORS
static int led_layer_listener_cb(const zmk_event_t *eh) {
if (initialized) {
update_layer_color();
}
return 0;
}

ZMK_LISTENER(led_layer_listener, led_layer_listener_cb);
ZMK_SUBSCRIPTION(led_layer_listener, zmk_layer_state_changed);
#endif

/* ===== main loop ===== */

extern void led_process_thread(void *d0, void *d1, void *d2) {
while (true) {
struct blink_item blink;
k_msgq_get(&led_msgq, &blink, K_FOREVER);

```
    if (blink.duration_ms > 0) {
        set_rgb_leds(blink.color, blink.duration_ms);
        set_rgb_leds(led_persistent_color, 50);
    } else {
        set_rgb_leds(blink.color, 0);
    }
}
```

}

K_THREAD_DEFINE(led_process_tid, 1024, led_process_thread,
NULL, NULL, NULL,
K_LOWEST_APPLICATION_THREAD_PRIO, 0, 100);

/* ===== init ===== */

extern void led_init_thread(void *d0, void *d1, void *d2) {

#if SHOW_LAYER_COLORS
update_layer_color();
#endif

#if IS_ENABLED(CONFIG_ZMK_SPLIT) && !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
update_battery_persistent_color();
#endif

```
initialized = true;
```

}

K_THREAD_DEFINE(led_init_tid, 1024, led_init_thread,
NULL, NULL, NULL,
K_LOWEST_APPLICATION_THREAD_PRIO, 0, 200);
