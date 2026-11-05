#include "companion_memory.h"
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#define SLOTS 8
#define TEXT_SIZE 161
typedef struct {unsigned version;char notes[SLOTS][TEXT_SIZE];} memory_data;
static memory_data current={.version=1};
static SemaphoreHandle_t guard;
static bool persisted;
static unsigned revision;
void companion_memory_init(void)
{
    guard=xSemaphoreCreateMutex();assert(guard);nvs_handle_t h;
    if(nvs_open("pavlov_memory",NVS_READONLY,&h)==ESP_OK){
        memory_data saved;size_t n=sizeof(saved);
        if(nvs_get_blob(h,"notes",&saved,&n)==ESP_OK&&n==sizeof(saved)&&saved.version==1){
            bool valid=true;for(int i=0;i<SLOTS;i++)if(!memchr(saved.notes[i],0,TEXT_SIZE))valid=false;
            if(valid){current=saved;persisted=true;}
        }
        nvs_close(h);
    }
}
cJSON *companion_memory_json(void)
{
    xSemaphoreTake(guard,portMAX_DELAY);cJSON *j=cJSON_CreateObject();cJSON *a=cJSON_AddArrayToObject(j,"notes");
    for(int i=0;i<SLOTS;i++)if(current.notes[i][0])cJSON_AddItemToArray(a,cJSON_CreateString(current.notes[i]));
    cJSON_AddNumberToObject(j,"capacity",SLOTS);cJSON_AddNumberToObject(j,"revision",revision);cJSON_AddBoolToObject(j,"persisted",persisted);
    xSemaphoreGive(guard);return j;
}
esp_err_t companion_memory_update(cJSON *body)
{
    cJSON *cmd=cJSON_GetObjectItemCaseSensitive(body,"command"),*value=cJSON_GetObjectItemCaseSensitive(body,"text"),*replacement=cJSON_GetObjectItemCaseSensitive(body,"replacement");
    if(!cJSON_IsString(cmd))return ESP_ERR_INVALID_ARG;
    bool clear=!strcmp(cmd->valuestring,"clear"),add=!strcmp(cmd->valuestring,"remember"),forget=!strcmp(cmd->valuestring,"forget"),replace=!strcmp(cmd->valuestring,"replace");
    if(!clear&&!add&&!forget&&!replace)return ESP_ERR_INVALID_ARG;
    if(!clear&&(!cJSON_IsString(value)||!value->valuestring[0]||strlen(value->valuestring)>=TEXT_SIZE))return ESP_ERR_INVALID_ARG;
    if(replace&&(!cJSON_IsString(replacement)||!replacement->valuestring[0]||strlen(replacement->valuestring)>=TEXT_SIZE))return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(guard,portMAX_DELAY);memory_data next=current;esp_err_t err=ESP_OK;int found=-1,empty=-1;
    if(clear)memset(next.notes,0,sizeof(next.notes));
    else{
        for(int i=0;i<SLOTS;i++){if(!next.notes[i][0]&&empty<0)empty=i;if(!strcmp(next.notes[i],value->valuestring))found=i;}
        if(add){if(found<0){if(empty<0){err=ESP_ERR_NO_MEM;goto done;}strcpy(next.notes[empty],value->valuestring);}}
        else if(found<0){err=ESP_ERR_NOT_FOUND;goto done;}
        else if(forget)next.notes[found][0]=0;
        else strcpy(next.notes[found],replacement->valuestring);
    }
    if(memcmp(&next,&current,sizeof(next))||!persisted){nvs_handle_t h;err=nvs_open("pavlov_memory",NVS_READWRITE,&h);
        if(!err){err=nvs_set_blob(h,"notes",&next,sizeof(next));if(!err)err=nvs_commit(h);nvs_close(h);}
        if(!err){current=next;persisted=true;++revision;}
    }
done:xSemaphoreGive(guard);return err;
}
bool companion_memory_voice(const char *text,char *reply,size_t capacity,bool *changed)
{
    *changed=false;char cleaned[1024];size_t n=0;
    while(*text&&n+1<sizeof(cleaned)){
        if(!strncmp(text,"，",3)||!strncmp(text,"。",3)||!strncmp(text,"！",3)){text+=3;continue;}
        cleaned[n++]=*text++;
    }
    cleaned[n]=0;char *s=cleaned;
    if(!strncmp(s,"巴甫洛夫",12))s+=12;
    if(!strncmp(s,"请",3))s+=3;
    if(!strcmp(s,"你记住了什么")||!strcmp(s,"查询记忆")||!strcmp(s,"查看记忆")){
        cJSON *j=companion_memory_json(),*a=cJSON_GetObjectItem(j,"notes");size_t used=0;int count=0;
        used=(size_t)snprintf(reply,capacity,"记得：");
        for(cJSON *p=a->child;p;p=p->next){size_t bytes=strlen(p->valuestring);if(used+bytes+40>=capacity)break;
            used+=(size_t)snprintf(reply+used,capacity-used,"%s%s",count?"；":"",p->valuestring);++count;}
        if(!a->child)snprintf(reply,capacity,"还没有保存事情。可以说记住我喜欢散步。");
        else if(count<cJSON_GetArraySize(a))snprintf(reply+used,capacity-used,"；其他记忆可在控制页查看。");
        cJSON_Delete(j);return true;
    }
    const char *command=NULL,*value=NULL,*replacement=NULL;char old[TEXT_SIZE];
    if(!strcmp(s,"清空记忆")||!strcmp(s,"忘记所有事情"))command="clear";
    else if(!strncmp(s,"忘记这件事",15)){command="forget";value=s+15;}
    else if(!strncmp(s,"忘记",6)){command="forget";value=s+6;}
    else if(!strncmp(s,"把",3)){
        char *split=strstr(s+3,"改成");if(split&&(size_t)(split-s-3)<sizeof(old)){
            memcpy(old,s+3,(size_t)(split-s-3));old[split-s-3]=0;command="replace";value=old;replacement=split+6;}
    }else if(!strncmp(s,"记住",6)){command="remember";value=s+6;}
    else if(!strncmp(s,"我喜欢",9)||!strncmp(s,"我不喜欢",12)){command="remember";value=s;}
    if(!command||strchr(s,'?')||strstr(s,"？")||strstr(s,"如果")||strstr(s,"假设"))return false;
    if(value==s&&(strstr(s,"什么")||strstr(s,"吗")||strstr(s,"是否")))return false;
    cJSON *j=cJSON_CreateObject();cJSON_AddStringToObject(j,"command",command);
    if(value)cJSON_AddStringToObject(j,"text",value);
    if(replacement)cJSON_AddStringToObject(j,"replacement",replacement);
    esp_err_t err=companion_memory_update(j);cJSON_Delete(j);
    if(err==ESP_ERR_NO_MEM)snprintf(reply,capacity,"记忆已满，先忘记一件旧事再添加。");
    else if(err==ESP_ERR_NOT_FOUND)snprintf(reply,capacity,"没有找到这条记忆，原有记忆保持不变。");
    else if(err)snprintf(reply,capacity,"这条记忆没有保存，请用五十字左右的短句。");
    else{*changed=true;snprintf(reply,capacity,!strcmp(command,"clear")?"已清空保存的事情，称呼和设置保留。":!strcmp(command,"forget")?"已忘记这件事。":!strcmp(command,"replace")?"已更新这条记忆。":"记住了。");}
    return true;
}
