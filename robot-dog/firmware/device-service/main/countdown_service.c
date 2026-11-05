#include "countdown_service.h"
#include "countdown.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdio.h>
#include <assert.h>
static SemaphoreHandle_t guard;
static countdown timer;
static unsigned notifications;
static int64_t now_ms(void){return esp_timer_get_time()/1000;}
void countdown_service_init(void){guard=xSemaphoreCreateMutex();assert(guard);}
void countdown_service_set(unsigned seconds)
{xSemaphoreTake(guard,portMAX_DELAY);countdown_set(&timer,seconds,now_ms());xSemaphoreGive(guard);}
bool countdown_service_try_set(unsigned seconds)
{
    xSemaphoreTake(guard,portMAX_DELAY);countdown_refresh(&timer,now_ms());
    bool idle=!timer.running&&!timer.pending&&(!timer.duration_seconds||timer.notified);
    if(idle)countdown_set(&timer,seconds,now_ms());
    xSemaphoreGive(guard);return idle;
}
void countdown_service_cancel(void)
{xSemaphoreTake(guard,portMAX_DELAY);countdown_cancel(&timer);xSemaphoreGive(guard);}
cJSON *countdown_service_json(void)
{
    xSemaphoreTake(guard,portMAX_DELAY);unsigned remaining=countdown_remaining(&timer,now_ms());countdown t=timer;unsigned total=notifications;xSemaphoreGive(guard);
    cJSON *j=cJSON_CreateObject();
    cJSON_AddStringToObject(j,"state",t.running?"running":t.pending?"pending":t.notified?"finished":t.duration_seconds?"alerting":"idle");
    cJSON_AddNumberToObject(j,"duration_seconds",t.duration_seconds);cJSON_AddNumberToObject(j,"remaining_seconds",remaining);
    cJSON_AddNumberToObject(j,"generation",t.generation);cJSON_AddBoolToObject(j,"notification_pending",t.pending);
    cJSON_AddBoolToObject(j,"notified",t.notified);cJSON_AddNumberToObject(j,"notifications",total);
    cJSON_AddNumberToObject(j,"alert_error",t.alert_error);cJSON_AddBoolToObject(j,"survives_restart",false);
    cJSON_AddBoolToObject(j,"offline_tone",true);cJSON_AddBoolToObject(j,"offline_voice",true);return j;
}
static void duration_text(unsigned seconds,char *out,size_t capacity)
{
    unsigned h=seconds/3600,m=seconds%3600/60,s=seconds%60;size_t used=0;
    if(h)used+=(size_t)snprintf(out+used,capacity-used,"%u小时",h);
    if(m)used+=(size_t)snprintf(out+used,capacity-used,"%u分钟",m);
    if(s||!used)snprintf(out+used,capacity-used,"%u秒",s);
}
bool countdown_service_voice(const char *text,char *reply,size_t capacity)
{
    unsigned seconds=0;int command=countdown_parse(text,&seconds);if(!command)return false;
    if(command==COUNTDOWN_HELP){snprintf(reply,capacity,"可以说三分钟后提醒我、还有多久、取消计时。计时在本机进行，重启会清除；断网到时也会提醒。");return true;}
    if(command==COUNTDOWN_INVALID){snprintf(reply,capacity,"计时范围是1秒到24小时，例如三分钟后提醒我。");return true;}
    char duration[64];xSemaphoreTake(guard,portMAX_DELAY);
    if(command==COUNTDOWN_SET){
        bool replace=timer.running||timer.pending;countdown_set(&timer,seconds,now_ms());duration_text(seconds,duration,sizeof(duration));
        snprintf(reply,capacity,replace?"已改为%s计时。":"已设置%s计时。",duration);
    }else if(command==COUNTDOWN_CANCEL){bool existed=timer.duration_seconds!=0;countdown_cancel(&timer);snprintf(reply,capacity,existed?"已取消计时。":"当前没有计时。");}
    else {unsigned remaining=countdown_remaining(&timer,now_ms());
        if(timer.running){duration_text(remaining,duration,sizeof(duration));snprintf(reply,capacity,"还剩%s。",duration);}
        else snprintf(reply,capacity,timer.duration_seconds?"计时已经结束。":"当前没有计时。");
    }
    xSemaphoreGive(guard);return true;
}
bool countdown_service_pending(void)
{xSemaphoreTake(guard,portMAX_DELAY);countdown_refresh(&timer,now_ms());bool due=timer.pending;xSemaphoreGive(guard);return due;}
unsigned countdown_service_take_due(void)
{xSemaphoreTake(guard,portMAX_DELAY);unsigned generation=countdown_take_due(&timer,now_ms())?timer.generation:0;xSemaphoreGive(guard);return generation;}
bool countdown_service_current(unsigned generation)
{xSemaphoreTake(guard,portMAX_DELAY);bool current=timer.generation==generation&&timer.duration_seconds>0;xSemaphoreGive(guard);return current;}
void countdown_service_finish(unsigned generation,int error)
{xSemaphoreTake(guard,portMAX_DELAY);if(timer.generation==generation){timer.notified=true;timer.alert_error=error;++notifications;}xSemaphoreGive(guard);}
