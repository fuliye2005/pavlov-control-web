#include "interaction.h"
#include "user_settings.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <assert.h>
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
static SemaphoreHandle_t guard;
static int current=PAVLOV_COMPANION;
static bool saved;
void interaction_init(void)
{
    guard=xSemaphoreCreateMutex();assert(guard);nvs_handle_t h;int32_t mode;
    if(nvs_open("pavlov_modes",NVS_READONLY,&h)==ESP_OK){
        if(nvs_get_i32(h,"mode",&mode)==ESP_OK&&mode>=0&&mode<=2){current=mode;saved=true;}
        nvs_close(h);
    }
}
int interaction_mode(void)
{xSemaphoreTake(guard,portMAX_DELAY);int mode=current;xSemaphoreGive(guard);return mode;}
const char *interaction_name(int mode)
{return mode==PAVLOV_REST?"rest":mode==PAVLOV_ACTION?"action":"companion";}
esp_err_t interaction_set(const char *name)
{
    int mode=!strcmp(name,"companion")?0:!strcmp(name,"action")?1:!strcmp(name,"rest")?2:-1;
    if(mode<0)return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(guard,portMAX_DELAY);esp_err_t err=ESP_OK;
    if(!saved||current!=mode){nvs_handle_t h;err=nvs_open("pavlov_modes",NVS_READWRITE,&h);
        if(!err){err=nvs_set_i32(h,"mode",mode);if(!err)err=nvs_commit(h);nvs_close(h);}
        if(!err){current=mode;saved=true;}}
    xSemaphoreGive(guard);return err;
}
cJSON *interaction_state(void)
{
    xSemaphoreTake(guard,portMAX_DELAY);int mode=current;bool persisted=saved;xSemaphoreGive(guard);
    cJSON *j=cJSON_CreateObject();cJSON_AddStringToObject(j,"interaction_mode",interaction_name(mode));
    cJSON_AddBoolToObject(j,"persisted",persisted);
    bool expressions=mode==PAVLOV_COMPANION&&user_settings_chat_actions();
    cJSON_AddBoolToObject(j,"autonomous_motion",expressions);
    cJSON_AddBoolToObject(j,"chat_expression_allowed",expressions);
    cJSON_AddBoolToObject(j,"autonomous_locomotion",false);
    cJSON_AddBoolToObject(j,"rest_requires_cloud_asr",true);return j;
}
bool interaction_voice(const char *text,char *reply,size_t cap)
{
    char b[1024];size_t n=0;
    while(*text&&n+1<sizeof(b)){
        unsigned char c=(unsigned char)*text;
        if(c<128&&(isspace(c)||ispunct(c))){++text;continue;}
        if(!strncmp(text,"，",3)||!strncmp(text,"。",3)||!strncmp(text,"！",3)||!strncmp(text,"？",3)){text+=3;continue;}
        b[n++]=c<128?(char)tolower(c):(char)c;++text;
    }
    /* Fun-ASR has actually transcribed the spoken English name as Pablof.
       This alias only affects mode commands, never the locomotion name gate. */
    b[n]=0;char *s=b;const char *prefix[]={"pavlov","pablof","pavelov","巴甫洛夫","帕夫洛夫","请","帮我"};
    for(int round=0;round<3;++round){bool found=false;
        for(unsigned k=0;k<sizeof(prefix)/sizeof(prefix[0]);++k){size_t len=strlen(prefix[k]);if(!strncmp(s,prefix[k],len)){s+=len;found=true;break;}}
        if(!found)break;
    }
    if(!strcmp(s,"报告模式")||!strcmp(s,"当前模式")||!strcmp(s,"现在什么模式")){
        int mode=interaction_mode();snprintf(reply,cap,"当前是%s模式。",mode==0?"陪伴":mode==1?"动作":"休息");return true;
    }
    static const struct {const char *command,*mode;} map[]={
        {"陪伴模式","companion"},{"进入陪伴模式","companion"},{"切换到陪伴模式","companion"},{"醒醒","companion"},
        {"动作模式","action"},{"进入动作模式","action"},{"切换到动作模式","action"},
        {"休息模式","rest"},{"进入休息模式","rest"},{"切换到休息模式","rest"},{"休息一下","rest"}};
    for(unsigned k=0;k<sizeof(map)/sizeof(map[0]);++k)if(!strcmp(s,map[k].command)){
        esp_err_t err=interaction_set(map[k].mode);
        if(err)snprintf(reply,cap,"模式保存失败，原模式保持不变。");
        else if(!strcmp(map[k].mode,"rest"))snprintf(reply,cap,"已进入休息模式。说巴甫洛夫醒醒恢复陪伴。");
        else if(!strcmp(map[k].mode,"action"))snprintf(reply,cap,"已进入动作模式，回应动作、帮助和设置口令。");
        else snprintf(reply,cap,"已进入陪伴模式。");
        return true;
    }
    return false;
}
