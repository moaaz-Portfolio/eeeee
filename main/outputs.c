#include "outputs.h"
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "led_strip.h"

#include "app.h"

static const char *TAG = "outputs";

#define BOOT_BUTTON_GPIO   0
#define TICK_MS            30
#define BUTTON_HOLD_MS     3000

static SemaphoreHandle_t s_mx;
static led_strip_handle_t s_strip;

/* snapshot of the settings we use (guarded by s_mx) */
static int s_led_type = 0, s_led_gpio = -1, s_bright = 40;
static uint8_t s_cr = 0, s_cg = 255, s_cb = 100;
static int s_out_gpio = -1, s_out_hold_s = 5;
static bool s_out_hi = true;

static volatile int s_net = OUT_NET_CONNECTING;
static volatile bool s_motion = false;
static volatile int64_t s_motion_off_us = INT64_MIN / 2;
static float s_level = 0.0f;
static int s_last_r = -1, s_last_g = -1, s_last_b = -1;
static int s_relay_state = -1;

/* ------------------------------------------------------------------ LED */

static void led_teardown(void)
{
    if (s_strip) { led_strip_clear(s_strip); led_strip_del(s_strip); s_strip = NULL; }
    if (s_led_type == 2 && s_led_gpio >= 0) gpio_reset_pin((gpio_num_t)s_led_gpio);
    s_last_r = s_last_g = s_last_b = -1;
}

static void led_setup(void)
{
    if (s_led_gpio < 0) return;
    if (s_led_type == 1) {
        led_strip_config_t sc = {
            .strip_gpio_num = s_led_gpio,
            .max_leds = 1,
            .led_model = LED_MODEL_WS2812,
            .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        };
        led_strip_rmt_config_t rc = { .resolution_hz = 10 * 1000 * 1000, .flags.with_dma = false };
        if (led_strip_new_rmt_device(&sc, &rc, &s_strip) != ESP_OK) {
            s_strip = NULL;
            ESP_LOGE(TAG, "WS2812 init failed on GPIO %d", s_led_gpio);
        } else {
            led_strip_clear(s_strip);
        }
    } else if (s_led_type == 2) {
        gpio_config_t g = { .pin_bit_mask = 1ULL << s_led_gpio, .mode = GPIO_MODE_OUTPUT };
        gpio_config(&g);
        gpio_set_level((gpio_num_t)s_led_gpio, 0);
    }
}

static void led_show(int r, int g, int b)
{
    if (r == s_last_r && g == s_last_g && b == s_last_b) return;
    s_last_r = r; s_last_g = g; s_last_b = b;
    if (s_led_type == 1 && s_strip) {
        led_strip_set_pixel(s_strip, 0, r, g, b);
        led_strip_refresh(s_strip);
    } else if (s_led_type == 2 && s_led_gpio >= 0) {
        gpio_set_level((gpio_num_t)s_led_gpio, (r | g | b) ? 1 : 0);
    }
}

/* ---------------------------------------------------------------- relay */

static void relay_teardown(void)
{
    if (s_out_gpio >= 0) gpio_reset_pin((gpio_num_t)s_out_gpio);
    s_relay_state = -1;
}

static void relay_setup(void)
{
    if (s_out_gpio < 0) return;
    gpio_config_t g = { .pin_bit_mask = 1ULL << s_out_gpio, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&g);
    gpio_set_level((gpio_num_t)s_out_gpio, s_out_hi ? 0 : 1);
    s_relay_state = 0;
}

static void relay_update(bool want)
{
    if (s_out_gpio < 0 || (int)want == s_relay_state) return;
    s_relay_state = want;
    gpio_set_level((gpio_num_t)s_out_gpio, (want == s_out_hi) ? 1 : 0);
}

/* ----------------------------------------------------------------- task */

static void parse_color(const char *hex)
{
    if (!hex || hex[0] != '#' || strlen(hex) != 7) return;
    long v = strtol(hex + 1, NULL, 16);
    s_cr = (v >> 16) & 0xFF; s_cg = (v >> 8) & 0xFF; s_cb = v & 0xFF;
}

static void snapshot(const settings_t *s)
{
    s_led_type = s->led_type;
    s_led_gpio = s->led_gpio;
    s_bright = s->led_brightness;
    parse_color(s->led_color);
    s_out_gpio = s->out_gpio;
    s_out_hi = s->out_active_high;
    s_out_hold_s = s->out_hold_s;
}

static void outputs_task(void *arg)
{
    int held_ms = 0;
    for (;;) {
        int64_t now_us = esp_timer_get_time();
        uint32_t now_ms = (uint32_t)(now_us / 1000);

        /* BOOT button: hold 3s to forget Wi-Fi */
        if (gpio_get_level(BOOT_BUTTON_GPIO) == 0) {
            held_ms += TICK_MS;
            if (held_ms >= BUTTON_HOLD_MS) {
                ESP_LOGW(TAG, "BOOT held: forgetting Wi-Fi");
                settings_wifi_reset();
                app_reboot_later(300);
                held_ms = -100000;
            }
        } else {
            held_ms = 0;
        }

        xSemaphoreTake(s_mx, portMAX_DELAY);

        float k = s_bright / 100.0f;
        int r = 0, g = 0, b = 0;
        if (s_net == OUT_NET_CONNECTING) {
            if ((now_ms / 400) & 1) { b = (int)(255 * 0.4f * k) + 1; }
        } else if (s_net == OUT_NET_SETUP) {
            if ((now_ms % 1500) < 300) { r = (int)(255 * k) + 1; g = (int)(110 * k); }
        } else {
            float target = s_motion ? 1.0f : 0.0f;
            s_level += (target - s_level) * 0.3f;
            if (!s_motion && s_level < 0.03f) s_level = 0.0f;
            float f = s_level * k;
            r = (int)(s_cr * f); g = (int)(s_cg * f); b = (int)(s_cb * f);
        }
        led_show(r, g, b);

        bool want = s_motion || (s_out_hold_s > 0 && (now_us - s_motion_off_us) < (int64_t)s_out_hold_s * 1000000LL);
        relay_update(want);

        xSemaphoreGive(s_mx);
        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
    }
}

/* ------------------------------------------------------------ public API */

void outputs_init(const settings_t *s)
{
    s_mx = xSemaphoreCreateMutex();

    gpio_config_t btn = { .pin_bit_mask = 1ULL << BOOT_BUTTON_GPIO, .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE };
    gpio_config(&btn);

    snapshot(s);
    led_setup();
    relay_setup();
    xTaskCreate(outputs_task, "outputs", 3072, NULL, 4, NULL);
}

void outputs_apply_settings(const settings_t *s)
{
    xSemaphoreTake(s_mx, portMAX_DELAY);
    led_teardown();
    relay_teardown();
    snapshot(s);
    led_setup();
    relay_setup();
    xSemaphoreGive(s_mx);
}

void outputs_set_net_state(int state) { s_net = state; }

void outputs_set_motion(bool motion)
{
    if (s_motion && !motion) s_motion_off_us = esp_timer_get_time();
    s_motion = motion;
}
