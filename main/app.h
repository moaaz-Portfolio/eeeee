#pragma once
#include <stdint.h>

#define FW_NAME    "ESP32-S3 WiFi Radar"
#define FW_VERSION "1.0.0"

/* Restart the chip after `ms` milliseconds (lets an HTTP reply finish first). */
void app_reboot_later(uint32_t ms);
