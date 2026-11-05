#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "cJSON.h"
void countdown_service_init(void);
void countdown_service_set(unsigned seconds);
bool countdown_service_try_set(unsigned seconds);
void countdown_service_cancel(void);
cJSON *countdown_service_json(void);
bool countdown_service_voice(const char *text,char *reply,size_t capacity);
bool countdown_service_pending(void);
unsigned countdown_service_take_due(void);
bool countdown_service_current(unsigned generation);
void countdown_service_finish(unsigned generation,int error);
