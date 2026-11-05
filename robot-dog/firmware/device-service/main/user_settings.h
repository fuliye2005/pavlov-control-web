#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "cJSON.h"
typedef struct {int version,volume,default_speed,default_strength;} user_preferences;
void user_settings_init(void);
user_preferences user_settings_get(void);
bool user_settings_chat_actions(void);
cJSON *user_settings_json(void);
esp_err_t user_settings_update(cJSON *patch);
bool user_settings_voice(const char *text,char *reply,size_t capacity);
