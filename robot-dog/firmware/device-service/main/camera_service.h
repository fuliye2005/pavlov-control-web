#pragma once
#include "cJSON.h"
#include "esp_err.h"
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
void camera_service_init(void);
bool camera_service_command(const char *command);
cJSON *camera_service_state(void);
esp_err_t camera_service_photo(uint8_t **data,size_t *length);
