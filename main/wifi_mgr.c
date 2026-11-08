#include "wifi_mgr.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "cJSON.h"
#include "dns_server.h"

#include "settings.h"
#include "outputs.h"

static const char *TAG = "wifi_mgr";

#define GOT_IP_BIT           BIT0
#define CONNECT_TIMEOUT_MS   25000
#define SETUP_IDLE_REBOOT_US (180LL * 1000000LL)
#define SETUP_AP_SSID        "S3-Radar-Setup"

static EventGroupHandle_t s_eg;
static esp_netif_t *s_sta, *s_ap;
static volatile bool s_try = false;
static volatile bool s_connected = false;
static bool s_started = false;
static netmode_t s_mode = NETMODE_STA;
static char s_ip[16] = "";
static volatile int64_t s_activity_us = 0;
static char *s_scan_cache = NULL;

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        if (id == WIFI_EVENT_STA_START) {
            if (s_try) esp_wifi_connect();
        } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
            s_connected = false;
            if (s_try) {
                if (s_mode == NETMODE_STA) outputs_set_net_state(OUT_NET_CONNECTING);
                esp_wifi_connect();
            }
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&ev->ip_info.ip));
        s_connected = true;
        ESP_LOGI(TAG, "connected, ip %s", s_ip);
        xEventGroupSetBits(s_eg, GOT_IP_BIT);
    }
}

/* ------------------------------------------------------------- scanning */

static void do_scan(void)
{
    wifi_scan_config_t sc = {0};
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) return;

    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n > 24) n = 24;
    wifi_ap_record_t *recs = calloc(n ? n : 1, sizeof(*recs));
    if (!recs) return;
    esp_wifi_scan_get_ap_records(&n, recs);

    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < n; i++) {
        const char *ssid = (const char *)recs[i].ssid;
        if (!ssid[0]) continue;
        bool dup = false;
        cJSON *it;
        cJSON_ArrayForEach(it, arr) {
            cJSON *s = cJSON_GetObjectItem(it, "ssid");
            if (s && !strcmp(s->valuestring, ssid)) { dup = true; break; }
        }
        if (dup) continue;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "ssid", ssid);
        cJSON_AddNumberToObject(o, "rssi", recs[i].rssi);
        cJSON_AddBoolToObject(o, "open", recs[i].authmode == WIFI_AUTH_OPEN);
        cJSON_AddItemToArray(arr, o);
    }
    free(s_scan_cache);
    s_scan_cache = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    free(recs);
}

const char *wifi_mgr_scan(bool refresh)
{
    if (refresh || !s_scan_cache) do_scan();
    return s_scan_cache ? s_scan_cache : "[]";
}

/* --------------------------------------------------------------- portal */

static void idle_task(void *arg)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        if (settings_has_wifi() && (esp_timer_get_time() - s_activity_us) > SETUP_IDLE_REBOOT_US) {
            ESP_LOGW(TAG, "setup portal idle, retrying saved Wi-Fi");
            esp_restart();
        }
    }
}

static esp_err_t start_portal(void)
{
    ESP_LOGW(TAG, "starting setup portal");
    s_try = false;
    s_mode = NETMODE_SETUP;
    s_activity_us = esp_timer_get_time();
    outputs_set_net_state(OUT_NET_SETUP);

    wifi_config_t ap = {0};
    strlcpy((char *)ap.ap.ssid, SETUP_AP_SSID, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = strlen(SETUP_AP_SSID);
    ap.ap.channel = 1;
    ap.ap.max_connection = 4;
    ap.ap.authmode = WIFI_AUTH_OPEN;

    if (s_started) esp_wifi_disconnect();
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    if (!s_started) { ESP_ERROR_CHECK(esp_wifi_start()); s_started = true; }

    /* Scan once now, before any phone joins, so the list is ready and the AP isn't disturbed later. */
    do_scan();

    /* Captive portal: every DNS name resolves to the ESP. */
    static dns_server_config_t dns_cfg = DNS_SERVER_CONFIG_SINGLE("*", "WIFI_AP_DEF");
    if (start_dns_server(&dns_cfg) == NULL) ESP_LOGE(TAG, "captive DNS failed");

    xTaskCreate(idle_task, "portal_idle", 2048, NULL, 3, NULL);
    ESP_LOGI(TAG, "join '%s' then open http://192.168.4.1", SETUP_AP_SSID);
    return ESP_OK;
}

/* ----------------------------------------------------------- public API */

netmode_t wifi_mgr_start(void)
{
    settings_t *c = settings_get();
    s_eg = xEventGroupCreate();
    s_sta = esp_netif_create_default_wifi_sta();
    s_ap = esp_netif_create_default_wifi_ap();
    (void)s_ap;

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    esp_netif_set_hostname(s_sta, c->hostname);

    if (settings_has_wifi()) {
        wifi_config_t sta = {0};
        strlcpy((char *)sta.sta.ssid, c->wifi_ssid, sizeof(sta.sta.ssid));
        strlcpy((char *)sta.sta.password, c->wifi_pass, sizeof(sta.sta.password));
        /* Accept open, WPA, WPA2 and WPA3 networks. */
        sta.sta.threshold.authmode = c->wifi_pass[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
        sta.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

        outputs_set_net_state(OUT_NET_CONNECTING);
        s_try = true;
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
        ESP_ERROR_CHECK(esp_wifi_start());
        s_started = true;
        ESP_LOGI(TAG, "connecting to '%s'", c->wifi_ssid);

        EventBits_t bits = xEventGroupWaitBits(s_eg, GOT_IP_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(CONNECT_TIMEOUT_MS));
        if (bits & GOT_IP_BIT) {
            esp_wifi_set_ps(WIFI_PS_NONE);   /* keeps CSI sampling steady */
            s_mode = NETMODE_STA;
            outputs_set_net_state(OUT_NET_OK);
            return NETMODE_STA;
        }
        ESP_LOGW(TAG, "could not join '%s'", c->wifi_ssid);
    }

    start_portal();
    return NETMODE_SETUP;
}

netmode_t wifi_mgr_mode(void) { return s_mode; }
const char *wifi_mgr_ip(void) { return s_mode == NETMODE_SETUP ? "192.168.4.1" : s_ip; }
bool wifi_mgr_connected(void) { return s_connected; }
void wifi_mgr_touch(void) { s_activity_us = esp_timer_get_time(); }
