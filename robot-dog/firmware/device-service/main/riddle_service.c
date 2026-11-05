#include "riddle_service.h"
#include "riddle.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdio.h>
#include <assert.h>
static SemaphoreHandle_t guard;
static riddle_game game;
void riddle_service_init(void){guard=xSemaphoreCreateMutex();assert(guard);}
cJSON *riddle_service_json(void)
{
    xSemaphoreTake(guard,portMAX_DELAY);riddle_game g=game;xSemaphoreGive(guard);
    cJSON *j=cJSON_CreateObject();cJSON_AddBoolToObject(j,"active",g.active);
    cJSON_AddNumberToObject(j,"round",g.round);cJSON_AddNumberToObject(j,"attempts",g.attempts);
    cJSON_AddNumberToObject(j,"correct_answers",g.correct_answers);
    cJSON_AddBoolToObject(j,"hinted",g.hinted);cJSON_AddBoolToObject(j,"revealed",g.revealed);cJSON_AddBoolToObject(j,"solved",g.solved);
    cJSON_AddStringToObject(j,"question",riddle_question(&g));cJSON_AddStringToObject(j,"hint",riddle_hint(&g));
    cJSON_AddStringToObject(j,"answer",riddle_answer(&g));cJSON_AddStringToObject(j,"last_reply",g.last_reply);
    cJSON_AddBoolToObject(j,"automatic_motion",false);cJSON_AddBoolToObject(j,"survives_restart",false);return j;
}
bool riddle_service_command(const char *command,const char *guess)
{xSemaphoreTake(guard,portMAX_DELAY);bool result=riddle_apply(&game,command,guess);xSemaphoreGive(guard);return result;}
bool riddle_service_voice(const char *text,char *reply,size_t capacity)
{xSemaphoreTake(guard,portMAX_DELAY);bool handled=riddle_voice(&game,text);if(handled)snprintf(reply,capacity,"%s",game.last_reply);xSemaphoreGive(guard);return handled;}
