#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "cJSON.h"
enum {PAVLOV_COMPANION=0,PAVLOV_ACTION=1,PAVLOV_REST=2};
void interaction_init(void);
int interaction_mode(void);
const char *interaction_name(int mode);
esp_err_t interaction_set(const char *mode);
cJSON *interaction_state(void);
bool interaction_voice(const char *text,char *reply,size_t capacity);
