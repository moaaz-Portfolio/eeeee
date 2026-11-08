#include "sensing.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_now.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_wifi_sensing.h"

#include "settings.h"
#include "outputs.h"
#include "notify.h"

static const char *TAG = "sensing";

static esp_wifi_sensing_fsm_handle_t s_fsm = NULL;
static uint8_t s_ap_mac[6];
static uint8_t s_pair_mac[6];
static bool s_has_pair = false;

static volatile bool s_ap_active = false, s_pair_active = false;
static bool s_motion = false, s_presence = false;
static int64_t s_last_motion_us = INT64_MIN / 2;
static sensing_snapshot_t s_snap;

static const uint8_t BROADCAST[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

/* ------------------------------------------------------------ callbacks */

static void on_event(esp_wifi_sensing_fsm_handle_t handle, const uint8_t peer_mac[6],
                     esp_wifi_sensing_fsm_event_t event, uint32_t data, void *usr)
{
    (void)handle; (void)data; (void)usr;
    bool active = (event == ESP_WIFI_SENSING_FSM_EVENT_ACTIVE);
    if (memcmp(peer_mac, s_ap_mac, 6) == 0) s_ap_active = active;
    else if (s_has_pair && memcmp(peer_mac, s_pair_mac, 6) == 0) s_pair_active = active;
}

static void espnow_rx_cb(const esp_now_recv_info_t *info, const uint8_t *d, int len)
{
    (void)info; (void)d; (void)len;   /* CSI is taken from the frame itself; payload is unused */
}

/* The primary channel feeds the graph, presence and calibration. */
static const uint8_t *primary_mac(void)
{
    return (settings_get()->source == 1 && s_has_pair) ? s_pair_mac : s_ap_mac;
}

/* ---------------------------------------------------------- logic task */

static void logic_task(void *arg)
{
    for (;;) {
        settings_t *c = settings_get();
        int64_t now = esp_timer_get_time();

        bool raw = (c->source == 0) ? s_ap_active
                 : (c->source == 1) ? s_pair_active
                 : (s_ap_active || s_pair_active);
        if (raw) s_last_motion_us = now;
        bool shown = raw || (now - s_last_motion_us) < (int64_t)c->hold_s * 1000000LL;

        if (shown != s_motion) {
            s_motion = shown;
            outputs_set_motion(shown);
            notify_post(NOTIFY_MOTION, shown);
        }

        esp_wifi_sensing_fsm_channel_diag_t diag = {0};
        if (s_fsm && esp_wifi_sensing_fsm_get_channel_diag(s_fsm, primary_mac(), &diag) == ESP_OK) {
            s_snap.jitter = diag.jitter_value;
            s_snap.wander = diag.wander_value;
            s_snap.ready = (diag.init_stage == ESP_WIFI_SENSING_FSM_INIT_STAGE_STABLE);
            s_snap.train_status = (int)diag.train_status;
            s_snap.train_valid = diag.train_thresholds_valid;
            bool pres = diag.presence_someone_status;
            if (pres != s_presence) {
                s_presence = pres;
                notify_post(NOTIFY_PRESENCE, pres);
            }
        }
        s_snap.motion = s_motion;
        s_snap.presence = s_presence;

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

/* ---------------------------------------------------------------- start */

esp_err_t sensing_start(void)
{
    settings_t *c = settings_get();

    wifi_ap_record_t ap = {0};
    esp_err_t e = esp_wifi_sta_get_ap_info(&ap);
    if (e != ESP_OK) { ESP_LOGE(TAG, "not connected to an AP"); return e; }
    memcpy(s_ap_mac, ap.bssid, 6);
    s_has_pair = settings_parse_mac(c->pair_mac, s_pair_mac);

    if (s_has_pair) {
        esp_err_t ne = esp_now_init();
        if (ne == ESP_OK || ne == ESP_ERR_ESPNOW_EXIST) {
            esp_now_register_recv_cb(espnow_rx_cb);
            if (!esp_now_is_peer_exist(BROADCAST)) {
                esp_now_peer_info_t p = {0};
                memcpy(p.peer_addr, BROADCAST, 6);
                p.channel = 0;
                p.ifidx = WIFI_IF_STA;
                esp_now_add_peer(&p);
            }
        }
    }

    esp_wifi_sensing_fsm_config_t cfg = DEFAULT_ESP_WIFI_SENSING_FSM_CONFIG();
    cfg.max_channel_num = s_has_pair ? 2 : 1;
    cfg.default_channel_config.sensitivity = c->sensitivity;
    cfg.default_channel_config.presence_sensitivity = c->presence_sensitivity;
    cfg.default_channel_config.active_jitter_min = c->jitter_min;
    cfg.default_channel_config.active_filter_ms = c->filter_ms;
    cfg.ping_frequency_hz = c->ping_hz;

    ESP_ERROR_CHECK(esp_wifi_sensing_fsm_create(&cfg, &s_fsm));
    ESP_ERROR_CHECK(esp_wifi_sensing_fsm_add_channel(s_fsm, s_ap_mac));
    if (s_has_pair) ESP_ERROR_CHECK(esp_wifi_sensing_fsm_add_channel(s_fsm, s_pair_mac));
    ESP_ERROR_CHECK(esp_wifi_sensing_fsm_register_event_cb(s_fsm, ESP_WIFI_SENSING_FSM_EVENT_ACTIVE, on_event, NULL));
    ESP_ERROR_CHECK(esp_wifi_sensing_fsm_register_event_cb(s_fsm, ESP_WIFI_SENSING_FSM_EVENT_INACTIVE, on_event, NULL));
    ESP_ERROR_CHECK(esp_wifi_sensing_fsm_control(s_fsm, ESP_WIFI_SENSING_FSM_CTRL_START, NULL));

    /* Router pings make the router answer, and every answer carries CSI. */
    esp_err_t pe = esp_wifi_sensing_fsm_ping_router_start(s_fsm);
    if (pe != ESP_OK) ESP_LOGW(TAG, "router ping not started: %s", esp_err_to_name(pe));

    s_snap.running = true;
    xTaskCreate(logic_task, "sense_logic", 4096, NULL, 4, NULL);
    ESP_LOGI(TAG, "sensing started (pair=%s, ping=%d Hz)", s_has_pair ? "yes" : "no", c->ping_hz);
    return ESP_OK;
}

/* --------------------------------------------------------------- sender */

static void sender_task(void *arg)
{
    uint32_t counter = 0;
    for (;;) {
        uint8_t hz = settings_get()->ping_hz;
        if (hz < 5) hz = 5;
        esp_now_send(BROADCAST, (uint8_t *)&counter, sizeof(counter));
        counter++;
        vTaskDelay(pdMS_TO_TICKS(1000 / hz));
    }
}

esp_err_t sender_start(void)
{
    esp_err_t e = esp_now_init();
    if (e != ESP_OK && e != ESP_ERR_ESPNOW_EXIST) return e;
    esp_now_peer_info_t p = {0};
    memcpy(p.peer_addr, BROADCAST, 6);
    p.channel = 0;            /* follow the router's channel */
    p.ifidx = WIFI_IF_STA;
    p.encrypt = false;
    if (!esp_now_is_peer_exist(BROADCAST)) esp_now_add_peer(&p);
    xTaskCreate(sender_task, "sender", 3072, NULL, 4, NULL);
    ESP_LOGI(TAG, "sender mode: transmitting for the receiver board");
    return ESP_OK;
}

/* ---------------------------------------------------- runtime controls */

void sensing_apply_settings(void)
{
    if (!s_fsm) return;
    settings_t *c = settings_get();
    const uint8_t *macs[2] = {s_ap_mac, s_pair_mac};
    for (int i = 0; i < (s_has_pair ? 2 : 1); i++) {
        esp_wifi_sensing_fsm_channel_config_t cc = {0};
        if (esp_wifi_sensing_fsm_get_channel_config(s_fsm, macs[i], &cc) != ESP_OK) continue;
        cc.sensitivity = c->sensitivity;
        cc.presence_sensitivity = c->presence_sensitivity;
        cc.active_jitter_min = c->jitter_min;
        cc.active_filter_ms = c->filter_ms;
        esp_wifi_sensing_fsm_set_channel_config(s_fsm, macs[i], &cc);
    }
}

void sensing_get(sensing_snapshot_t *out) { *out = s_snap; }

esp_err_t sensing_train_start(void)
{
    return s_fsm ? esp_wifi_sensing_fsm_train_start(s_fsm, primary_mac()) : ESP_ERR_INVALID_STATE;
}

esp_err_t sensing_train_stop(float *w, float *j)
{
    return s_fsm ? esp_wifi_sensing_fsm_train_stop(s_fsm, primary_mac(), w, j) : ESP_ERR_INVALID_STATE;
}

esp_err_t sensing_train_remove(void)
{
    return s_fsm ? esp_wifi_sensing_fsm_train_remove(s_fsm, primary_mac()) : ESP_ERR_INVALID_STATE;
}

esp_err_t sensing_reset_baseline(void)
{
    return s_fsm ? esp_wifi_sensing_fsm_control(s_fsm, ESP_WIFI_SENSING_FSM_CTRL_RESET_BASELINE, NULL) : ESP_ERR_INVALID_STATE;
}
