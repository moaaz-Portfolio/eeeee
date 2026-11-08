#include "settings.h"
#include <ctype.h>
#include <stdlib.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_mac.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_wifi_sensing.h"

static const char *TAG = "settings";
#define NVS_NS  "radar"
#define NVS_KEY "cfg"

static settings_t s_cfg;

settings_t *settings_get(void) { return &s_cfg; }

bool settings_parse_mac(const char *str, uint8_t out[6])
{
    if (!str) return false;
    unsigned v[6];
    if (sscanf(str, "%2x:%2x:%2x:%2x:%2x:%2x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) return false;
    for (int i = 0; i < 6; i++) out[i] = (uint8_t)v[i];
    return true;
}

void settings_defaults(settings_t *o)
{
    memset(o, 0, sizeof(*o));

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(o->hostname, sizeof(o->hostname), "s3radar-%02x%02x%02x", mac[3], mac[4], mac[5]);

    /* Take the component's own defaults so we never guess its scales. */
    esp_wifi_sensing_fsm_config_t c = DEFAULT_ESP_WIFI_SENSING_FSM_CONFIG();
    o->sensitivity = 0.75f;   /* the "responsive" value DrWalls used */
    o->presence_sensitivity = c.default_channel_config.presence_sensitivity;
    o->jitter_min = c.default_channel_config.active_jitter_min;
    o->filter_ms = c.default_channel_config.active_filter_ms;
    o->ping_hz = (c.ping_frequency_hz > 0 && c.ping_frequency_hz <= 200) ? (uint8_t)c.ping_frequency_hz : 50;
    o->hold_s = 3;

    o->role = 0;
    o->stream_port = 5005;
    o->source = 0;

    o->led_type = 1;         /* DevKitC-1 has a WS2812 on GPIO48 */
    o->led_gpio = 48;
    o->led_brightness = 40;
    strcpy(o->led_color, "#00ff66");

    o->out_gpio = -1;
    o->out_active_high = true;
    o->out_hold_s = 5;

    strcpy(o->lang, "ar");

    o->mqtt_port = 1883;
    strcpy(o->mqtt_prefix, "wifi-radar");
    o->mqtt_ha = true;
}

/* ---------------------------------------------------------------- JSON */

static double round4(float v) { return floor((double)v * 10000.0 + 0.5) / 10000.0; }

cJSON *settings_to_json(const settings_t *c, bool secrets)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "wifi_ssid", c->wifi_ssid);
    cJSON_AddStringToObject(o, "hostname", c->hostname);
    cJSON_AddNumberToObject(o, "role", c->role);
    cJSON_AddStringToObject(o, "stream_host", c->stream_host);
    cJSON_AddNumberToObject(o, "stream_port", c->stream_port);
    cJSON_AddStringToObject(o, "pair_mac", c->pair_mac);
    cJSON_AddNumberToObject(o, "source", c->source);
    cJSON_AddNumberToObject(o, "sensitivity", round4(c->sensitivity));
    cJSON_AddNumberToObject(o, "presence_sensitivity", round4(c->presence_sensitivity));
    cJSON_AddNumberToObject(o, "jitter_min", round4(c->jitter_min));
    cJSON_AddNumberToObject(o, "filter_ms", c->filter_ms);
    cJSON_AddNumberToObject(o, "ping_hz", c->ping_hz);
    cJSON_AddNumberToObject(o, "hold_s", c->hold_s);
    cJSON_AddNumberToObject(o, "led_type", c->led_type);
    cJSON_AddNumberToObject(o, "led_gpio", c->led_gpio);
    cJSON_AddNumberToObject(o, "led_brightness", c->led_brightness);
    cJSON_AddStringToObject(o, "led_color", c->led_color);
    cJSON_AddNumberToObject(o, "out_gpio", c->out_gpio);
    cJSON_AddBoolToObject(o, "out_active_high", c->out_active_high);
    cJSON_AddNumberToObject(o, "out_hold_s", c->out_hold_s);
    cJSON_AddStringToObject(o, "lang", c->lang);
    cJSON_AddBoolToObject(o, "mqtt_en", c->mqtt_en);
    cJSON_AddStringToObject(o, "mqtt_host", c->mqtt_host);
    cJSON_AddNumberToObject(o, "mqtt_port", c->mqtt_port);
    cJSON_AddStringToObject(o, "mqtt_user", c->mqtt_user);
    cJSON_AddStringToObject(o, "mqtt_prefix", c->mqtt_prefix);
    cJSON_AddBoolToObject(o, "mqtt_ha", c->mqtt_ha);
    cJSON_AddStringToObject(o, "webhook_url", c->webhook_url);
    if (secrets) {
        cJSON_AddStringToObject(o, "wifi_pass", c->wifi_pass);
        cJSON_AddStringToObject(o, "ui_pass", c->ui_pass);
        cJSON_AddStringToObject(o, "mqtt_pass", c->mqtt_pass);
    } else {
        cJSON_AddBoolToObject(o, "has_ui_pass", c->ui_pass[0] != 0);
        cJSON_AddBoolToObject(o, "has_mqtt_pass", c->mqtt_pass[0] != 0);
    }
    return o;
}

static const cJSON *item(const cJSON *j, const char *k) { return cJSON_GetObjectItemCaseSensitive(j, k); }

static bool get_str(const cJSON *j, const char *k, char *dst, size_t n)
{
    const cJSON *i = item(j, k);
    if (cJSON_IsString(i) && i->valuestring) { strlcpy(dst, i->valuestring, n); return true; }
    return false;
}

static void sanitize_host(char *h)
{
    size_t w = 0;
    for (size_t i = 0; h[i] && w < 31; i++) {
        char ch = (char)tolower((unsigned char)h[i]);
        if (isalnum((unsigned char)ch) || ch == '-') h[w++] = ch;
    }
    h[w] = 0;
    if (w == 0) strcpy(h, "s3radar");
}

static float clampf(const cJSON *i, float lo, float hi, float cur)
{
    if (!cJSON_IsNumber(i)) return cur;
    float v = (float)i->valuedouble;
    return v < lo ? lo : (v > hi ? hi : v);
}

static int clampi(const cJSON *i, int lo, int hi, int cur)
{
    if (!cJSON_IsNumber(i)) return cur;
    int v = (int)i->valuedouble;
    return v < lo ? lo : (v > hi ? hi : v);
}

static bool getb(const cJSON *i, bool cur)
{
    return cJSON_IsBool(i) ? cJSON_IsTrue(i) : cur;
}

void settings_apply_json(settings_t *c, const cJSON *j, bool trusted)
{
    if (!j) return;
    char tmp[130];

    if (trusted) {
        get_str(j, "wifi_ssid", c->wifi_ssid, sizeof(c->wifi_ssid));
        get_str(j, "wifi_pass", c->wifi_pass, sizeof(c->wifi_pass));
        get_str(j, "ui_pass", c->ui_pass, sizeof(c->ui_pass));
        get_str(j, "mqtt_pass", c->mqtt_pass, sizeof(c->mqtt_pass));
    } else {
        if (get_str(j, "ui_pass", tmp, sizeof(c->ui_pass)) && tmp[0]) strlcpy(c->ui_pass, tmp, sizeof(c->ui_pass));
        if (cJSON_IsTrue(item(j, "ui_pass_clear"))) c->ui_pass[0] = 0;
        if (get_str(j, "mqtt_pass", tmp, sizeof(c->mqtt_pass)) && tmp[0]) strlcpy(c->mqtt_pass, tmp, sizeof(c->mqtt_pass));
        if (cJSON_IsTrue(item(j, "mqtt_pass_clear"))) c->mqtt_pass[0] = 0;
    }

    if (get_str(j, "hostname", c->hostname, sizeof(c->hostname))) sanitize_host(c->hostname);

    c->role = (uint8_t)clampi(item(j, "role"), 0, 2, c->role);
    if (get_str(j, "stream_host", tmp, sizeof(c->stream_host))) {
        bool ok = true;
        for (const char *p = tmp; *p; p++) if (!((*p >= '0' && *p <= '9') || *p == '.')) ok = false;
        if (ok) strlcpy(c->stream_host, tmp, sizeof(c->stream_host));
    }
    c->stream_port = (uint16_t)clampi(item(j, "stream_port"), 1, 65535, c->stream_port);
    if (get_str(j, "pair_mac", tmp, sizeof(c->pair_mac))) {
        uint8_t m[6];
        if (tmp[0] == 0) c->pair_mac[0] = 0;
        else if (settings_parse_mac(tmp, m)) snprintf(c->pair_mac, sizeof(c->pair_mac), "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
    }
    c->source = (uint8_t)clampi(item(j, "source"), 0, 2, c->source);
    if (c->source != 0 && c->pair_mac[0] == 0) c->source = 0;   /* nothing to listen to */

    c->sensitivity = clampf(item(j, "sensitivity"), 0.001f, 1.0f, c->sensitivity);
    c->presence_sensitivity = clampf(item(j, "presence_sensitivity"), 0.0f, 1.0f, c->presence_sensitivity);
    c->jitter_min = clampf(item(j, "jitter_min"), 0.0f, 100.0f, c->jitter_min);
    c->filter_ms = (uint32_t)clampi(item(j, "filter_ms"), 0, 600000, (int)c->filter_ms);
    c->ping_hz = (uint8_t)clampi(item(j, "ping_hz"), 5, 200, c->ping_hz);
    c->hold_s = (uint16_t)clampi(item(j, "hold_s"), 0, 3600, c->hold_s);

    c->led_type = (uint8_t)clampi(item(j, "led_type"), 0, 2, c->led_type);
    c->led_gpio = (int8_t)clampi(item(j, "led_gpio"), 0, 48, c->led_gpio);
    c->led_brightness = (uint8_t)clampi(item(j, "led_brightness"), 1, 100, c->led_brightness);
    if (get_str(j, "led_color", tmp, 8) && strlen(tmp) == 7 && tmp[0] == '#') strlcpy(c->led_color, tmp, sizeof(c->led_color));

    c->out_gpio = (int8_t)clampi(item(j, "out_gpio"), -1, 48, c->out_gpio);
    c->out_active_high = getb(item(j, "out_active_high"), c->out_active_high);
    c->out_hold_s = (uint16_t)clampi(item(j, "out_hold_s"), 0, 3600, c->out_hold_s);

    if (get_str(j, "lang", tmp, 3) && (!strcmp(tmp, "ar") || !strcmp(tmp, "en"))) strlcpy(c->lang, tmp, sizeof(c->lang));

    c->mqtt_en = getb(item(j, "mqtt_en"), c->mqtt_en);
    get_str(j, "mqtt_host", c->mqtt_host, sizeof(c->mqtt_host));
    c->mqtt_port = (uint16_t)clampi(item(j, "mqtt_port"), 1, 65535, c->mqtt_port);
    get_str(j, "mqtt_user", c->mqtt_user, sizeof(c->mqtt_user));
    if (get_str(j, "mqtt_prefix", tmp, sizeof(c->mqtt_prefix)) && tmp[0]) {
        for (char *p = tmp; *p; p++) if (*p == '#' || *p == '+' || *p == ' ') *p = '-';
        strlcpy(c->mqtt_prefix, tmp, sizeof(c->mqtt_prefix));
    }
    c->mqtt_ha = getb(item(j, "mqtt_ha"), c->mqtt_ha);

    if (get_str(j, "webhook_url", tmp, sizeof(c->webhook_url))) {
        if (tmp[0] == 0 || !strncmp(tmp, "http://", 7) || !strncmp(tmp, "https://", 8)) strlcpy(c->webhook_url, tmp, sizeof(c->webhook_url));
    }
}

/* ----------------------------------------------------------------- NVS */

esp_err_t settings_load(void)
{
    settings_defaults(&s_cfg);

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return ESP_OK;

    size_t len = 0;
    if (nvs_get_str(h, NVS_KEY, NULL, &len) == ESP_OK && len > 1 && len < 4096) {
        char *buf = malloc(len);
        if (buf && nvs_get_str(h, NVS_KEY, buf, &len) == ESP_OK) {
            cJSON *j = cJSON_Parse(buf);
            if (j) { settings_apply_json(&s_cfg, j, true); cJSON_Delete(j); }
            else ESP_LOGW(TAG, "stored settings are corrupt, using defaults");
        }
        free(buf);
    }
    nvs_close(h);
    return ESP_OK;
}

esp_err_t settings_save(void)
{
    cJSON *j = settings_to_json(&s_cfg, true);
    char *txt = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (!txt) return ESP_ERR_NO_MEM;

    nvs_handle_t h;
    esp_err_t e = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (e == ESP_OK) {
        e = nvs_set_str(h, NVS_KEY, txt);
        if (e == ESP_OK) e = nvs_commit(h);
        nvs_close(h);
    }
    cJSON_free(txt);
    if (e != ESP_OK) ESP_LOGE(TAG, "save failed: %s", esp_err_to_name(e));
    return e;
}

esp_err_t settings_wifi_reset(void)
{
    s_cfg.wifi_ssid[0] = 0;
    s_cfg.wifi_pass[0] = 0;
    return settings_save();
}

esp_err_t settings_reset_all(void)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    nvs_erase_all(h);
    e = nvs_commit(h);
    nvs_close(h);
    settings_defaults(&s_cfg);
    return e;
}

bool settings_has_wifi(void) { return s_cfg.wifi_ssid[0] != 0; }
