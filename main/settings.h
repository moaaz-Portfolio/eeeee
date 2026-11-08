#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "cJSON.h"

typedef struct {
    /* Network */
    char wifi_ssid[33];
    char wifi_pass[65];
    char hostname[32];
    /* Mode */
    uint8_t role;            /* 0 = sensor, 1 = sender (helper board), 2 = CSI streamer (for ML tools) */
    char stream_host[40];    /* PC address for role 2, "" = broadcast */
    uint16_t stream_port;    /* UDP port for role 2 */
    char pair_mac[18];       /* sender MAC "aa:bb:cc:dd:ee:ff" or "" */
    uint8_t source;          /* 0 = router link, 1 = paired sender, 2 = either */
    /* Detection */
    float sensitivity;       /* motion, 0.001 .. 1 */
    float presence_sensitivity; /* 0 .. 1 */
    float jitter_min;
    uint32_t filter_ms;
    uint8_t ping_hz;         /* router ping / sender packet rate */
    uint16_t hold_s;         /* keep "motion" on screen for N seconds */
    /* LED */
    uint8_t led_type;        /* 0 none, 1 WS2812 (RGB), 2 plain GPIO LED */
    int8_t led_gpio;
    uint8_t led_brightness;  /* 1..100 */
    char led_color[8];       /* "#rrggbb" */
    /* Output pin (relay / buzzer) */
    int8_t out_gpio;         /* -1 = disabled */
    bool out_active_high;
    uint16_t out_hold_s;
    /* UI */
    char lang[3];            /* "ar" / "en" */
    char ui_pass[33];        /* "" = open dashboard (user is "admin") */
    /* MQTT */
    bool mqtt_en;
    char mqtt_host[64];
    uint16_t mqtt_port;
    char mqtt_user[33];
    char mqtt_pass[65];
    char mqtt_prefix[32];
    bool mqtt_ha;            /* Home Assistant auto-discovery */
    /* Webhook */
    char webhook_url[128];
} settings_t;

settings_t *settings_get(void);
void settings_defaults(settings_t *s);
esp_err_t settings_load(void);
esp_err_t settings_save(void);
esp_err_t settings_wifi_reset(void);   /* forget Wi-Fi only */
esp_err_t settings_reset_all(void);    /* factory reset */
bool settings_has_wifi(void);

/* trusted=true (NVS load) may also set Wi-Fi credentials and clear secrets. */
cJSON *settings_to_json(const settings_t *s, bool include_secrets);
void settings_apply_json(settings_t *s, const cJSON *j, bool trusted);

bool settings_parse_mac(const char *str, uint8_t out[6]);
