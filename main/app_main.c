#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "mdns.h"

#include "app.h"
#include "settings.h"
#include "outputs.h"
#include "wifi_mgr.h"
#include "web_server.h"
#include "sensing.h"
#include "notify.h"
#include "csi_stream.h"

static const char *TAG = "app";

static void reboot_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS((uint32_t)(uintptr_t)arg));
    esp_restart();
}

void app_reboot_later(uint32_t ms)
{
    xTaskCreate(reboot_task, "reboot", 2048, (void *)(uintptr_t)ms, 5, NULL);
}

static void start_mdns(const settings_t *c)
{
    if (mdns_init() != ESP_OK) { ESP_LOGW(TAG, "mdns init failed"); return; }
    mdns_hostname_set(c->hostname);
    mdns_instance_name_set(FW_NAME);
    mdns_service_add(FW_NAME, "_http", "_tcp", 80, NULL, 0);
    ESP_LOGI(TAG, "open http://%s.local", c->hostname);
}

void app_main(void)
{
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        e = nvs_flash_init();
    }
    ESP_ERROR_CHECK(e);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    settings_load();
    settings_t *c = settings_get();

    outputs_init(c);

    netmode_t mode = wifi_mgr_start();
    web_server_start();

    if (mode == NETMODE_STA) {
        start_mdns(c);
        if (c->role == 1) sender_start();
        else if (c->role == 2) csi_stream_start();
        else sensing_start();
        if (c->role != 2) notify_start();
    } else {
        ESP_LOGW(TAG, "setup mode: connect to the 'S3-Radar-Setup' Wi-Fi");
    }
}
