#pragma once
#include <stdint.h>
#include "esp_err.h"

/* Role 2: stream raw CSI amplitudes (64 subcarriers) over UDP for the Python tools. */
esp_err_t csi_stream_start(void);
uint32_t csi_stream_sent(void);
