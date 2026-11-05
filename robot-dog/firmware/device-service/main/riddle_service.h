#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "cJSON.h"
void riddle_service_init(void);
cJSON *riddle_service_json(void);
bool riddle_service_command(const char *command,const char *guess);
bool riddle_service_voice(const char *text,char *reply,size_t capacity);
