#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "cJSON.h"
#include "esp_err.h"
void companion_memory_init(void);
cJSON *companion_memory_json(void);
esp_err_t companion_memory_update(cJSON *body);
bool companion_memory_voice(const char *text,char *reply,size_t capacity,bool *changed);
