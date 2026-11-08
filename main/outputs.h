#pragma once
#include <stdbool.h>
#include "settings.h"

enum { OUT_NET_CONNECTING = 0, OUT_NET_OK = 1, OUT_NET_SETUP = 2 };

void outputs_init(const settings_t *s);            /* LED + output pin + BOOT button */
void outputs_apply_settings(const settings_t *s);  /* live re-configure */
void outputs_set_net_state(int state);
void outputs_set_motion(bool motion);
