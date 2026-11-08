#pragma once
#include <stdbool.h>
#include "esp_err.h"

typedef enum { NETMODE_STA = 0, NETMODE_SETUP = 1 } netmode_t;

/* Connect with saved credentials, or open the setup portal. Blocks until decided. */
netmode_t wifi_mgr_start(void);
netmode_t wifi_mgr_mode(void);
const char *wifi_mgr_ip(void);
bool wifi_mgr_connected(void);

/* JSON array text: [{"ssid":"..","rssi":-50,"open":false}, ...]. Valid until next call. */
const char *wifi_mgr_scan(bool refresh);

/* Called on every portal request so the idle-reboot timer is reset. */
void wifi_mgr_touch(void);
