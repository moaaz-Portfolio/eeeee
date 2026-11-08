#pragma once
#include <stdbool.h>

typedef enum { NOTIFY_MOTION = 0, NOTIFY_PRESENCE = 1 } notify_type_t;

void notify_start(void);        /* MQTT client + webhook worker */
void notify_reconfigure(void);  /* re-read MQTT / webhook settings */
void notify_post(notify_type_t type, bool value);
bool notify_mqtt_connected(void);
