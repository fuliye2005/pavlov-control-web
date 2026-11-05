#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "cJSON.h"
typedef struct {uint32_t version;char nickname[73];uint8_t brief;} user_profile;
void user_profile_init(void);
user_profile user_profile_get(void);
unsigned user_profile_revision(void);
cJSON *user_profile_json(void);
esp_err_t user_profile_update(cJSON *patch);
bool user_profile_voice(const char *text,char *reply,size_t capacity,bool *changed);
