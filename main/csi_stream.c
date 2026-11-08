#include "csi_stream.h"
#include <math.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "lwip/ip_addr.h"
#include "lwip/sockets.h"
#include "ping/ping_sock.h"

#include "settings.h"
#include "outputs.h"

static const char *TAG = "csi_stream";

#define CSI_SUBCARRIERS 64

/* Wire format (little endian): "CSI1" | seq u32 | ts_ms u32 | rssi i8 | n u8 | amp[n] u8 */
#pragma pack(push, 1)
typedef struct {
    char magic[4];
    uint32_t seq;
    uint32_t ts_ms;
    int8_t rssi;
    uint8_t n;
    uint8_t amp[CSI_SUBCARRIERS];
} csi_pkt_t;
#pragma pack(pop)

static QueueHandle_t s_q;
static uint8_t s_bssid[6];
static volatile uint32_t s_seq = 0;
static volatile uint32_t s_sent = 0;

uint32_t csi_stream_sent(void) { return s_sent; }

/* Runs inside the Wi-Fi task: keep it tiny, just convert and enqueue. */
static void csi_cb(void *ctx, wifi_csi_info_t *info)
{
    (void)ctx;
    if (!info || !info->buf || info->len < 2 * CSI_SUBCARRIERS) return;
    if (memcmp(info->mac, s_bssid, 6) != 0) return;   /* only frames coming from the router */

    csi_pkt_t p;
    memcpy(p.magic, "CSI1", 4);
    p.seq = s_seq++;
    p.ts_ms = (uint32_t)(esp_timer_get_time() / 1000);
    p.rssi = info->rx_ctrl.rssi;
    p.n = CSI_SUBCARRIERS;

    const int8_t *b = (const int8_t *)info->buf;   /* pairs of (imag, real) */
    for (int i = 0; i < CSI_SUBCARRIERS; i++) {
        float im = b[2 * i], re = b[2 * i + 1];
        float a = sqrtf(re * re + im * im) + 0.5f;
        p.amp[i] = a > 255.0f ? 255 : (uint8_t)a;
    }
    xQueueSend(s_q, &p, 0);
}

static void send_task(void *arg)
{
    settings_t *c = settings_get();
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) { ESP_LOGE(TAG, "socket failed"); vTaskDelete(NULL); }

    int yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));

    struct sockaddr_in dst = {0};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(c->stream_port ? c->stream_port : 5005);
    if (c->stream_host[0] == 0 || inet_pton(AF_INET, c->stream_host, &dst.sin_addr) != 1) {
        dst.sin_addr.s_addr = htonl(INADDR_BROADCAST);   /* zero-config: the PC just listens */
        ESP_LOGI(TAG, "streaming to broadcast :%u", ntohs(dst.sin_port));
    } else {
        ESP_LOGI(TAG, "streaming to %s:%u", c->stream_host, ntohs(dst.sin_port));
    }

    csi_pkt_t p;
    for (;;) {
        if (xQueueReceive(s_q, &p, portMAX_DELAY) == pdTRUE) {
            if (sendto(sock, &p, sizeof(p), 0, (struct sockaddr *)&dst, sizeof(dst)) > 0) s_sent++;
        }
    }
}

/* Every echo reply from the router carries a fresh CSI sample. */
static void start_ping(uint8_t hz)
{
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip;
    if (!sta || esp_netif_get_ip_info(sta, &ip) != ESP_OK || ip.gw.addr == 0) {
        ESP_LOGE(TAG, "no gateway to ping");
        return;
    }
    esp_ping_config_t pc = ESP_PING_DEFAULT_CONFIG();
    pc.target_addr.type = IPADDR_TYPE_V4;
    pc.target_addr.u_addr.ip4.addr = ip.gw.addr;
    pc.count = ESP_PING_COUNT_INFINITE;
    pc.interval_ms = 1000 / (hz ? hz : 50);
    pc.timeout_ms = 500;

    esp_ping_callbacks_t cbs = {0};
    esp_ping_handle_t h = NULL;
    if (esp_ping_new_session(&pc, &cbs, &h) == ESP_OK) esp_ping_start(h);
    else ESP_LOGE(TAG, "ping session failed");
}

esp_err_t csi_stream_start(void)
{
    settings_t *c = settings_get();

    wifi_ap_record_t ap;
    esp_err_t e = esp_wifi_sta_get_ap_info(&ap);
    if (e != ESP_OK) return e;
    memcpy(s_bssid, ap.bssid, 6);

    s_q = xQueueCreate(64, sizeof(csi_pkt_t));
    if (!s_q) return ESP_ERR_NO_MEM;

    /* LLTF only = 64 subcarriers, available on every frame type. Raw, no smoothing. */
    wifi_csi_config_t cfg = {
        .lltf_en = true,
        .htltf_en = false,
        .stbc_htltf2_en = false,
        .ltf_merge_en = true,
        .channel_filter_en = false,
        .manu_scale = false,
        .shift = 0,
        .dump_ack_en = false,
    };
    ESP_ERROR_CHECK(esp_wifi_set_csi_config(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_csi_rx_cb(csi_cb, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_csi(true));

    xTaskCreate(send_task, "csi_send", 4096, NULL, 5, NULL);
    start_ping(c->ping_hz);
    outputs_set_motion(true);   /* LED stays lit while streaming */
    ESP_LOGI(TAG, "CSI streaming started (%u Hz pings)", c->ping_hz);
    return ESP_OK;
}
