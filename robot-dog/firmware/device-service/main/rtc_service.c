#include "rtc_service.h"
#include "countdown_service.h"
#include <stdio.h>
#include <string.h>
bool rtc_service_local_time(dog_rtc_date *local,bool *available)
{
    dog_rtc_snapshot s;motion_bridge_rtc_snapshot(&s);*available=s.available;
    if(!s.valid||s.pending)return false;
    uint64_t time=(uint64_t)dog_rtc_seconds(&s.utc)+s.age_ms/1000+8*3600;
    return time<3155760000ull&&dog_rtc_from_seconds((uint32_t)time,local);
}
void rtc_service_reply(char *text,size_t capacity)
{
    dog_rtc_date local;bool available;
    if(!rtc_service_local_time(&local,&available)){snprintf(text,capacity,available?"机内时钟还没校好，请先在控制页对时。":"现在读不到有效的机内时间，请稍后再查。");return;}
    const char *days[]={"日","一","二","三","四","五","六"};
    snprintf(text,capacity,"机内时钟显示，北京时间%u年%u月%u日，星期%s，%u点%02u分。",local.year,local.month,local.day,days[local.weekday],local.hour,local.minute);
}
cJSON *rtc_service_reminder(const char *utc,int *status)
{
    dog_rtc_date target;const char *error=NULL;*status=200;
    if(!utc||!dog_rtc_parse_iso(utc,strlen(utc),&target)){error="invalid_rtc_utc";*status=422;}
    bool reserved=false;
    if(!error){error=motion_bridge_ota_reserve();reserved=!error;if(error)*status=409;}
    if(!error){
        dog_rtc_snapshot s;motion_bridge_rtc_snapshot(&s);
        if(!s.valid||s.pending){error="rtc_reminder_requires_valid_time";*status=409;}
        else{
            int64_t delay=(int64_t)dog_rtc_seconds(&target)-dog_rtc_seconds(&s.utc)-s.age_ms/1000;
            if(delay<5||delay>86400){error="rtc_reminder_range";*status=422;}
            else if(!countdown_service_try_set((unsigned)delay)){error="timer_already_active";*status=409;}
        }
    }
    if(reserved)motion_bridge_ota_release();
    if(error){cJSON *j=cJSON_CreateObject();cJSON_AddStringToObject(j,"error",error);return j;}
    cJSON *j=countdown_service_json();cJSON_AddStringToObject(j,"source","rtc_snapshot_to_monotonic");cJSON_AddStringToObject(j,"utc_target",utc);return j;
}
