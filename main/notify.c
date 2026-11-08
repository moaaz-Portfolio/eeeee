#include "notify.h"
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "mqtt_client.h"
#include "cJSON.h"

#include "app.h"
#include "settings.h"

static const char *TAG = "notify";

typedef struct { notify_type_t type; bool value; } evt_t;

static QueueHandle_t s_q;
static esp_mqtt_client_handle_t s_mqtt;
static volatile bool s_mq_up = false;
static bool s_motion = false, s_presence = false;
static char s_base[96], s_avail[112];

bool notify_mqtt_connected(void) { return s_mq_up; }

/* ---------------------------------------------------------------- MQTT */

static void mq_pub(const char *sub, const char *val, bool retain)
{
    if (!s_mqtt || !s_mq_up) return;
    char topic[140];
    snprintf(topic, sizeof(topic), "%s/%s", s_base, sub);
    esp_mqtt_client_publish(s_mqtt, topic, val, 0, 1, retain);
}

static void ha_discovery_one(const char *key, const char *name, const char *dev_class)
{
    settings_t *c = settings_get();
    cJSON *o = cJSON_CreateObject();
    char buf[160];

    cJSON_AddStringToObject(o, "name", name);
    snprintf(buf, sizeof(buf), "%s_%s", c->hostname, key);
    cJSON_AddStringToObject(o, "unique_id", buf);
    snprintf(buf, sizeof(buf), "%s/%s", s_base, key);
    cJSON_AddStringToObject(o, "state_topic", buf);
    cJSON_AddStringToObject(o, "payload_on", "ON");
    cJSON_AddStringToObject(o, "payload_off", "OFF");
    cJSON_AddStringToObject(o, "device_class", dev_class);
    cJSON_AddStringToObject(o, "availability_topic", s_avail);

    cJSON *dev = cJSON_AddObjectToObject(o, "device");
    cJSON *ids = cJSON_AddArrayToObject(dev, "identifiers");
    cJSON_AddItemToArray(ids, cJSON_CreateString(c->hostname));
    cJSON_AddStringToObject(dev, "name", c->hostname);
    cJSON_AddStringToObject(dev, "model", FW_NAME);
    cJSON_AddStringToObject(dev, "sw_version", FW_VERSION);

    char *txt = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (txt) {
        char topic[160];
        snprintf(topic, sizeof(topic), "homeassistant/binary_sensor/%s_%s/config", c->hostname, key);
        esp_mqtt_client_publish(s_mqtt, topic, txt, 0, 1, true);
        cJSON_free(txt);
    }
}

static void mq_publish_states(void)
{
    mq_pub("motion", s_motion ? "ON" : "OFF", true);
    mq_pub("presence", s_presence ? "ON" : "OFF", true);
}

static void mq_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == MQTT_EVENT_CONNECTED) {
        s_mq_up = true;
        ESP_LOGI(TAG, "MQTT connected");
        esp_mqtt_client_publish(s_mqtt, s_avail, "online", 0, 1, true);
        if (settings_get()->mqtt_ha) {
            ha_discovery_one("motion", "Motion", "motion");
            ha_discovery_one("presence", "Presence", "occupancy");
        }
        mq_publish_states();
    } else if (id == MQTT_EVENT_DISCONNECTED) {
        s_mq_up = false;
    }
}

static void mqtt_stop(void)
{
    if (s_mqtt) {
        s_mq_up = false;
        esp_mqtt_client_stop(s_mqtt);
        esp_mqtt_client_destroy(s_mqtt);
        s_mqtt = NULL;
    }
}

static void mqtt_begin(void)
{
    settings_t *c = settings_get();
    if (!c->mqtt_en || c->mqtt_host[0] == 0) return;

    snprintf(s_base, sizeof(s_base), "%s/%s", c->mqtt_prefix, c->hostname);
    snprintf(s_avail, sizeof(s_avail), "%s/status", s_base);

    char uri[112];
    snprintf(uri, sizeof(uri), "mqtt://%s:%u", c->mqtt_host, c->mqtt_port);

    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = uri,
        .credentials.username = c->mqtt_user[0] ? c->mqtt_user : NULL,
        .credentials.authentication.password = c->mqtt_pass[0] ? c->mqtt_pass : NULL,
        .credentials.client_id = c->hostname,
        .session.last_will.topic = s_avail,
        .session.last_will.msg = "offline",
        .session.last_will.qos = 1,
        .session.last_will.retain = 1,
    };
    s_mqtt = esp_mqtt_client_init(&cfg);
    if (!s_mqtt) return;
    esp_mqtt_client_register_event(s_mqtt, ESP_EVENT_ANY_ID, mq_event, NULL);
    esp_mqtt_client_start(s_mqtt);
}

/* -------------------------------------------------------------- webhook */

static void webhook_send(const char *event, bool state)
{
    settings_t *c = settings_get();
    if (c->webhook_url[0] == 0) return;

    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "device", c->hostname);
    cJSON_AddStringToObject(o, "event", event);
    cJSON_AddBoolToObject(o, "state", state);
    cJSON_AddNumberToObject(o, "uptime_s", (double)(esp_timer_get_time() / 1000000LL));
    char *body = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!body) return;

    esp_http_client_config_t hc = {
        .url = c->webhook_url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 4000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t cl = esp_http_client_init(&hc);
    if (cl) {
        esp_http_client_set_header(cl, "Content-Type", "application/json");
        esp_http_client_set_post_field(cl, body, strlen(body));
        esp_err_t e = esp_http_client_perform(cl);
        if (e != ESP_OK) ESP_LOGW(TAG, "webhook failed: %s", esp_err_to_name(e));
        esp_http_client_cleanup(cl);
    }
    cJSON_free(body);
}

/* ---------------------------------------------------------------- worker */

static void worker(void *arg)
{
    evt_t e;
    for (;;) {
        if (xQueueReceive(s_q, &e, portMAX_DELAY) != pdTRUE) continue;
        const char *name = (e.type == NOTIFY_MOTION) ? "motion" : "presence";
        mq_pub(name, e.value ? "ON" : "OFF", true);
        webhook_send(name, e.value);
    }
}

void notify_post(notify_type_t type, bool value)
{
    if (type == NOTIFY_MOTION) s_motion = value; else s_presence = value;
    if (!s_q) return;
    evt_t e = { type, value };
    xQueueSend(s_q, &e, 0);
}

void notify_start(void)
{
    s_q = xQueueCreate(16, sizeof(evt_t));
    xTaskCreate(worker, "notify", 6144, NULL, 3, NULL);
    mqtt_begin();
}

void notify_reconfigure(void)
{
    if (!s_q) return;   /* not started (setup mode) */
    mqtt_stop();
    mqtt_begin();
}
