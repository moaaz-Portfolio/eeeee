#pragma once
#include <stdbool.h>
#include "esp_err.h"

typedef struct {
    bool running;
    bool ready;        /* detector finished warming up */
    bool motion;
    bool presence;
    float jitter;
    float wander;
    int train_status;  /* 0 none, 1 in progress, 2 complete */
    bool train_valid;
} sensing_snapshot_t;

esp_err_t sensing_start(void);     /* receiver: watches router link (+ optional paired sender) */
esp_err_t sender_start(void);      /* helper board: just transmits packets for the receiver */
void sensing_apply_settings(void); /* push detection settings live */
void sensing_get(sensing_snapshot_t *out);

esp_err_t sensing_train_start(void);
esp_err_t sensing_train_stop(float *wander_th, float *jitter_th);
esp_err_t sensing_train_remove(void);
esp_err_t sensing_reset_baseline(void);
