#pragma once
#include "cJSON.h"
void board_controls_init(void);
cJSON *board_controls_state(void);
cJSON *board_controls_probe_storage(int *status);
