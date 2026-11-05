#pragma once
#include "motion_bridge.h"
#include <stddef.h>
bool rtc_service_local_time(dog_rtc_date *local,bool *available);
void rtc_service_reply(char *text,size_t capacity);
cJSON *rtc_service_reminder(const char *utc,int *status);
