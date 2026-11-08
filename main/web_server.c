#include "web_server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "mbedtls/base64.h"
#include "cJSON.h"

#include "app.h"
#include "settings.h"
#include "wifi_mgr.h"
#include "outputs.h"
#include "sensing.h"
#include "notify.h"
#include "csi_stream.h"

static const char *TAG = "web";

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[]   asm("_binary_index_html_end");
extern const char setup_html_start[] asm("_binary_setup_html_start");
extern const char setup_html_end[]   asm("_binary_setup_html_end");

/* ------------------------------------------------------------- helpers */

static bool setup_mode(void) { return wifi_mgr_mode() == NETMODE_SETUP; }

static bool authorized(httpd_req_t *req)
{
    const settings_t *c = settings_get();
    if (setup_mode() || c->ui_pass[0] == 0) return true;

    char cred[64], enc[100], expect[110], hdr[128];
    snprintf(cred, sizeof(cred), "admin:%s", c->ui_pass);
    size_t olen = 0;
    if (mbedtls_base64_encode((unsigned char *)enc, sizeof(enc) - 1, &olen, (const unsigned char *)cred, strlen(cred)) != 0) return false;
    enc[olen] = 0;
    snprintf(expect, sizeof(expect), "Basic %s", enc);

    if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) != ESP_OK) return false;
    return strcmp(hdr, expect) == 0;
}

/* Returns false (after replying) when the request must not proceed. */
static bool guard(httpd_req_t *req, bool is_post)
{
    if (setup_mode()) wifi_mgr_touch();
    if (!authorized(req)) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"WiFi Radar\"");
        httpd_resp_sendstr(req, "Unauthorized");
        return false;
    }
    /* Custom header = browsers refuse to send it cross-site, which blocks CSRF. */
    if (is_post && httpd_req_get_hdr_value_len(req, "X-Radar") == 0) {
        httpd_resp_set_status(req, "403 Forbidden");
        httpd_resp_sendstr(req, "Forbidden");
        return false;
    }
    return true;
}

static esp_err_t send_json(httpd_req_t *req, cJSON *o)
{
    char *s = cJSON_PrintUnformatted(o);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t e = httpd_resp_send(req, s ? s : "{}", HTTPD_RESP_USE_STRLEN);
    if (s) cJSON_free(s);
    cJSON_Delete(o);
    return e;
}

static esp_err_t send_ok(httpd_req_t *req)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    return send_json(req, o);
}

static esp_err_t send_fail(httpd_req_t *req, const char *msg)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", false);
    cJSON_AddStringToObject(o, "error", msg);
    return send_json(req, o);
}

static cJSON *read_json(httpd_req_t *req, size_t max)
{
    size_t len = req->content_len;
    if (len == 0 || len > max) return NULL;
    char *buf = malloc(len + 1);
    if (!buf) return NULL;
    size_t got = 0;
    while (got < len) {
        int r = httpd_req_recv(req, buf + got, len - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) { free(buf); return NULL; }
        got += r;
    }
    buf[len] = 0;
    cJSON *j = cJSON_Parse(buf);
    free(buf);
    return j;
}

/* --------------------------------------------------------------- pages */

static esp_err_t page_handler(httpd_req_t *req)
{
    if (!guard(req, false)) return ESP_OK;
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    if (setup_mode())
        return httpd_resp_send(req, setup_html_start, setup_html_end - setup_html_start - 1);
    return httpd_resp_send(req, index_html_start, index_html_end - index_html_start - 1);
}

static esp_err_t catchall_handler(httpd_req_t *req)
{
    if (setup_mode()) {
        wifi_mgr_touch();   /* captive portal: bounce every unknown URL to the setup page */
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
        return httpd_resp_send(req, NULL, 0);
    }
    httpd_resp_send_404(req);
    return ESP_OK;
}

/* ----------------------------------------------------------- /api/status */

static esp_err_t status_handler(httpd_req_t *req)
{
    if (!guard(req, false)) return ESP_OK;
    const settings_t *c = settings_get();
    sensing_snapshot_t sn;
    sensing_get(&sn);

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char macs[18];
    snprintf(macs, sizeof(macs), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "mode", setup_mode() ? "setup" : "sta");
    cJSON_AddStringToObject(o, "fw", FW_VERSION);
    cJSON_AddStringToObject(o, "name", FW_NAME);
    cJSON_AddStringToObject(o, "mac", macs);
    cJSON_AddStringToObject(o, "ip", wifi_mgr_ip());
    cJSON_AddStringToObject(o, "hostname", c->hostname);
    cJSON_AddNumberToObject(o, "role", c->role);
    cJSON_AddNumberToObject(o, "uptime", (double)(esp_timer_get_time() / 1000000LL));
    cJSON_AddNumberToObject(o, "heap", (double)esp_get_free_heap_size());

    wifi_ap_record_t ap;
    if (!setup_mode() && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        cJSON_AddNumberToObject(o, "rssi", ap.rssi);
        cJSON_AddStringToObject(o, "ssid", (const char *)ap.ssid);
    }

    cJSON_AddBoolToObject(o, "running", sn.running);
    cJSON_AddBoolToObject(o, "ready", sn.ready);
    cJSON_AddBoolToObject(o, "motion", sn.motion);
    cJSON_AddBoolToObject(o, "presence", sn.presence);
    cJSON_AddNumberToObject(o, "jitter", (double)sn.jitter);
    cJSON_AddNumberToObject(o, "wander", (double)sn.wander);
    cJSON_AddNumberToObject(o, "train_status", sn.train_status);
    cJSON_AddBoolToObject(o, "train_valid", sn.train_valid);
    cJSON_AddBoolToObject(o, "mqtt", notify_mqtt_connected());
    cJSON_AddNumberToObject(o, "csi_sent", csi_stream_sent());
    return send_json(req, o);
}

/* ----------------------------------------------------------- /api/config */

static esp_err_t config_get_handler(httpd_req_t *req)
{
    if (!guard(req, false)) return ESP_OK;
    return send_json(req, settings_to_json(settings_get(), false));
}

static esp_err_t config_post_handler(httpd_req_t *req)
{
    if (!guard(req, true)) return ESP_OK;
    cJSON *j = read_json(req, 2048);
    if (!j) return send_fail(req, "bad json");

    settings_t *c = settings_get();
    settings_t old = *c;
    settings_apply_json(c, j, false);
    cJSON_Delete(j);
    settings_save();

    outputs_apply_settings(c);
    sensing_apply_settings();
    notify_reconfigure();

    bool reboot = old.role != c->role || old.ping_hz != c->ping_hz ||
                  strcmp(old.pair_mac, c->pair_mac) != 0 || strcmp(old.stream_host, c->stream_host) != 0 ||
                  old.stream_port != c->stream_port || strcmp(old.hostname, c->hostname) != 0;

    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddBoolToObject(o, "reboot_needed", reboot);
    return send_json(req, o);
}

/* ------------------------------------------------------------- Wi-Fi API */

static esp_err_t scan_handler(httpd_req_t *req)
{
    if (!guard(req, false)) return ESP_OK;
    char q[24] = {0};
    bool refresh = false;
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) refresh = strstr(q, "refresh=1") != NULL;
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, wifi_mgr_scan(refresh));
}

static esp_err_t wifi_post_handler(httpd_req_t *req)
{
    if (!guard(req, true)) return ESP_OK;
    cJSON *j = read_json(req, 1024);
    if (!j) return send_fail(req, "bad json");

    const cJSON *ssid = cJSON_GetObjectItemCaseSensitive(j, "ssid");
    const cJSON *pass = cJSON_GetObjectItemCaseSensitive(j, "password");
    if (!cJSON_IsString(ssid) || !ssid->valuestring[0] || strlen(ssid->valuestring) > 32) {
        cJSON_Delete(j);
        return send_fail(req, "invalid ssid");
    }
    if (cJSON_IsString(pass) && strlen(pass->valuestring) > 64) {
        cJSON_Delete(j);
        return send_fail(req, "password too long");
    }

    settings_t *c = settings_get();
    strlcpy(c->wifi_ssid, ssid->valuestring, sizeof(c->wifi_ssid));
    strlcpy(c->wifi_pass, cJSON_IsString(pass) ? pass->valuestring : "", sizeof(c->wifi_pass));
    /* optional extras sent by the setup page */
    settings_apply_json(c, j, false);
    cJSON_Delete(j);

    if (settings_save() != ESP_OK) return send_fail(req, "save failed");
    ESP_LOGI(TAG, "Wi-Fi saved for '%s', restarting", c->wifi_ssid);
    send_ok(req);
    app_reboot_later(1500);
    return ESP_OK;
}

/* ------------------------------------------------------- calibration API */

static esp_err_t calibrate_handler(httpd_req_t *req)
{
    if (!guard(req, true)) return ESP_OK;
    cJSON *j = read_json(req, 128);
    const cJSON *a = j ? cJSON_GetObjectItemCaseSensitive(j, "action") : NULL;
    const char *act = cJSON_IsString(a) ? a->valuestring : "";

    esp_err_t e = ESP_ERR_INVALID_ARG;
    float w = 0, jt = 0;
    bool stopped = false;
    if (!strcmp(act, "start")) e = sensing_train_start();
    else if (!strcmp(act, "stop")) { e = sensing_train_stop(&w, &jt); stopped = (e == ESP_OK); }
    else if (!strcmp(act, "clear")) e = sensing_train_remove();
    else if (!strcmp(act, "baseline")) e = sensing_reset_baseline();
    if (j) cJSON_Delete(j);

    if (e != ESP_OK) return send_fail(req, esp_err_to_name(e));
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    if (stopped) {
        cJSON_AddNumberToObject(o, "wander", (double)w);
        cJSON_AddNumberToObject(o, "jitter", (double)jt);
    }
    return send_json(req, o);
}

/* ---------------------------------------------------------- system API */

static esp_err_t reboot_handler(httpd_req_t *req)
{
    if (!guard(req, true)) return ESP_OK;
    send_ok(req);
    app_reboot_later(800);
    return ESP_OK;
}

static esp_err_t reset_wifi_handler(httpd_req_t *req)
{
    if (!guard(req, true)) return ESP_OK;
    settings_wifi_reset();
    send_ok(req);
    app_reboot_later(800);
    return ESP_OK;
}

static esp_err_t factory_handler(httpd_req_t *req)
{
    if (!guard(req, true)) return ESP_OK;
    settings_reset_all();
    send_ok(req);
    app_reboot_later(800);
    return ESP_OK;
}

static esp_err_t ota_handler(httpd_req_t *req)
{
    if (!guard(req, true)) return ESP_OK;
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part || req->content_len == 0 || req->content_len > part->size) return send_fail(req, "bad image size");

    esp_ota_handle_t h;
    if (esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &h) != ESP_OK) return send_fail(req, "ota begin failed");

    char *buf = malloc(1024);
    if (!buf) { esp_ota_abort(h); return send_fail(req, "no memory"); }

    size_t left = req->content_len;
    while (left > 0) {
        int r = httpd_req_recv(req, buf, left > 1024 ? 1024 : left);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0 || esp_ota_write(h, buf, r) != ESP_OK) {
            free(buf);
            esp_ota_abort(h);
            return send_fail(req, "upload failed");
        }
        left -= r;
    }
    free(buf);

    if (esp_ota_end(h) != ESP_OK) return send_fail(req, "invalid firmware image");
    if (esp_ota_set_boot_partition(part) != ESP_OK) return send_fail(req, "set boot failed");

    ESP_LOGI(TAG, "OTA complete, rebooting");
    send_ok(req);
    app_reboot_later(1000);
    return ESP_OK;
}

static esp_err_t favicon_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

/* --------------------------------------------------------------- start */

esp_err_t web_server_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 24;
    cfg.stack_size = 8192;
    cfg.recv_wait_timeout = 10;
    cfg.lru_purge_enable = true;
    cfg.uri_match_fn = httpd_uri_match_wildcard;

    httpd_handle_t srv = NULL;
    esp_err_t e = httpd_start(&srv, &cfg);
    if (e != ESP_OK) { ESP_LOGE(TAG, "httpd_start: %s", esp_err_to_name(e)); return e; }

    static const httpd_uri_t routes[] = {
        { .uri = "/",                   .method = HTTP_GET,  .handler = page_handler },
        { .uri = "/favicon.ico",        .method = HTTP_GET,  .handler = favicon_handler },
        { .uri = "/api/status",         .method = HTTP_GET,  .handler = status_handler },
        { .uri = "/api/config",         .method = HTTP_GET,  .handler = config_get_handler },
        { .uri = "/api/config",         .method = HTTP_POST, .handler = config_post_handler },
        { .uri = "/api/scan",           .method = HTTP_GET,  .handler = scan_handler },
        { .uri = "/api/wifi",           .method = HTTP_POST, .handler = wifi_post_handler },
        { .uri = "/api/calibrate",      .method = HTTP_POST, .handler = calibrate_handler },
        { .uri = "/api/reboot",         .method = HTTP_POST, .handler = reboot_handler },
        { .uri = "/api/reset-wifi",     .method = HTTP_POST, .handler = reset_wifi_handler },
        { .uri = "/api/factory-reset",  .method = HTTP_POST, .handler = factory_handler },
        { .uri = "/api/ota",            .method = HTTP_POST, .handler = ota_handler },
        /* must be last: wildcard catch-all */
        { .uri = "/*",                  .method = HTTP_GET,  .handler = catchall_handler },
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        ESP_ERROR_CHECK(httpd_register_uri_handler(srv, &routes[i]));
    }
    ESP_LOGI(TAG, "web server ready");
    return ESP_OK;
}
