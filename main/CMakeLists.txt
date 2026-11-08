idf_component_register(
    SRCS "app_main.c"
         "settings.c"
         "wifi_mgr.c"
         "web_server.c"
         "sensing.c"
         "outputs.c"
         "notify.c"
         "csi_stream.c"
    INCLUDE_DIRS "."
    EMBED_TXTFILES "web/index.html" "web/setup.html"
    REQUIRES esp_wifi esp_netif esp_event nvs_flash esp_timer driver
             esp_http_server esp_http_client esp-tls json mqtt app_update
             mbedtls lwip esp_wifi_sensing led_strip mdns dns_server
)
