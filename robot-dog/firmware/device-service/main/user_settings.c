#include "user_settings.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <assert.h>
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
static SemaphoreHandle_t guard;
static user_preferences current={1,35,2,25};
static bool persisted;
/* Separate key preserves the version-1 preference blob layout. */
static bool chat_actions=true;
static bool valid(user_preferences s)
{return s.version==1&&s.volume>=0&&s.volume<=100&&s.default_speed>=1&&s.default_speed<=3&&s.default_strength>=25&&s.default_strength<=100;}
void user_settings_init(void)
{
    guard=xSemaphoreCreateMutex();assert(guard);nvs_handle_t h;
    if(nvs_open("pavlov_prefs",NVS_READONLY,&h)==ESP_OK){
        user_preferences s;size_t n=sizeof(s);
        if(nvs_get_blob(h,"preferences",&s,&n)==ESP_OK&&n==sizeof(s)&&valid(s)){current=s;persisted=true;}
        uint8_t value;if(nvs_get_u8(h,"chat_actions",&value)==ESP_OK&&value<=1)chat_actions=value;
        nvs_close(h);
    }
}
user_preferences user_settings_get(void)
{xSemaphoreTake(guard,portMAX_DELAY);user_preferences s=current;xSemaphoreGive(guard);return s;}
bool user_settings_chat_actions(void)
{xSemaphoreTake(guard,portMAX_DELAY);bool value=chat_actions;xSemaphoreGive(guard);return value;}
cJSON *user_settings_json(void)
{
    xSemaphoreTake(guard,portMAX_DELAY);user_preferences s=current;bool saved=persisted,expressions=chat_actions;xSemaphoreGive(guard);
    cJSON *j=cJSON_CreateObject();cJSON_AddNumberToObject(j,"volume",s.volume);
    cJSON_AddStringToObject(j,"default_speed",s.default_speed==1?"slow":s.default_speed==3?"fast":"normal");
    cJSON_AddNumberToObject(j,"default_strength",s.default_strength);cJSON_AddBoolToObject(j,"persisted",saved);
    cJSON_AddNumberToObject(j,"voice_strength_limit",100);cJSON_AddBoolToObject(j,"chat_actions",expressions);
    cJSON_AddStringToObject(j,"volume_apply","next_playback");return j;
}
esp_err_t user_settings_update(cJSON *patch)
{
    if(!cJSON_IsObject(patch)||!patch->child)return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(guard,portMAX_DELAY);user_preferences s=current;bool expressions=chat_actions;esp_err_t err=ESP_OK;
    for(cJSON *p=patch->child;p;p=p->next){
        for(cJSON *q=p->next;q;q=q->next)if(p->string&&q->string&&!strcmp(p->string,q->string)){err=ESP_ERR_INVALID_ARG;goto done;}
        if(p->string&&!strcmp(p->string,"chat_actions")){
            if(!cJSON_IsBool(p)){err=ESP_ERR_INVALID_ARG;goto done;}expressions=cJSON_IsTrue(p);
        }else if(p->string&&!strcmp(p->string,"default_speed")){
            if(!cJSON_IsString(p)){err=ESP_ERR_INVALID_ARG;goto done;}
            s.default_speed=!strcmp(p->valuestring,"slow")?1:!strcmp(p->valuestring,"normal")?2:!strcmp(p->valuestring,"fast")?3:0;
        }else if(p->string&&(!strcmp(p->string,"volume")||!strcmp(p->string,"default_strength"))){
            if(!cJSON_IsNumber(p)||p->valuedouble<0||p->valuedouble>100||(int)p->valuedouble!=p->valuedouble){err=ESP_ERR_INVALID_ARG;goto done;}
            if(!strcmp(p->string,"volume"))s.volume=(int)p->valuedouble;else s.default_strength=(int)p->valuedouble;
        }else{err=ESP_ERR_INVALID_ARG;goto done;}
    }
    if(!valid(s)){err=ESP_ERR_INVALID_ARG;goto done;}
    if(persisted&&!memcmp(&s,&current,sizeof(s))&&expressions==chat_actions)goto done;
    nvs_handle_t h;err=nvs_open("pavlov_prefs",NVS_READWRITE,&h);
    if(!err){err=nvs_set_blob(h,"preferences",&s,sizeof(s));if(!err)err=nvs_set_u8(h,"chat_actions",expressions?1:0);if(!err)err=nvs_commit(h);nvs_close(h);}
    if(!err){current=s;chat_actions=expressions;persisted=true;}
done:
    xSemaphoreGive(guard);return err;
}
static bool percentage(const char *s,int *value)
{
    char b[64];if(strlen(s)>=sizeof(b))return false;snprintf(b,sizeof(b),"%s",s);
    if(!strncmp(b,"百分之",9))memmove(b,b+9,strlen(b+9)+1);
    size_t n=strlen(b);if(n&&b[n-1]=='%')b[--n]=0;
    if(!n)return false;
    bool digits=true;for(size_t k=0;k<n;++k)if(!isdigit((unsigned char)b[k]))digits=false;
    if(digits){long v=strtol(b,NULL,10);if(v<0||v>100)return false;*value=(int)v;return true;}
    const char *names[]={"零","一","二","三","四","五","六","七","八","九"};
    if(!strcmp(b,"一百")){*value=100;return true;}
    for(int tens=0;tens<10;++tens)for(int units=0;units<10;++units){
        char word[32];
        if(!tens)snprintf(word,sizeof(word),"%s",names[units]);
        else snprintf(word,sizeof(word),"%s十%s",tens==1?"":names[tens],units?names[units]:"");
        if(!strcmp(word,b)){*value=tens*10+units;return true;}
    }
    return false;
}
bool user_settings_voice(const char *text,char *reply,size_t cap)
{
    char b[1024];size_t n=0;
    while(*text&&n+1<sizeof(b)){
        if(isspace((unsigned char)*text)){++text;continue;}
        if(!strncmp(text,"，",3)||!strncmp(text,"。",3)||!strncmp(text,"！",3)||!strncmp(text,"？",3)){text+=3;continue;}
        b[n++]=*text++;
    }
    b[n]=0;while(n&&(b[n-1]=='.'||b[n-1]=='!'||b[n-1]=='?'))b[--n]=0;
    char *s=b;const char *prefix[]={"Pavlov","pavlov","巴甫洛夫","帕夫洛夫","请","帮我"};
    for(int round=0;round<3;++round){bool found=false;
        for(unsigned k=0;k<sizeof(prefix)/sizeof(prefix[0]);++k){size_t len=strlen(prefix[k]);if(!strncmp(s,prefix[k],len)){s+=len;found=true;break;}}
        if(!found)break;
    }
    user_preferences prefs=user_settings_get();
    if(!strcmp(s,"报告设置")||!strcmp(s,"查询设置")||!strcmp(s,"音量是多少")){
        snprintf(reply,cap,"音量百分之%d，默认动作%s，默认幅度百分之%d。当前带线执行上限百分之二十五。",prefs.volume,
            prefs.default_speed==1?"慢速":prefs.default_speed==3?"快速":"正常速度",prefs.default_strength);return true;
    }
    cJSON *patch=cJSON_CreateObject();bool recognized=true,invalid=false;
    if(!strcmp(s,"关闭聊天动作")||!strcmp(s,"安静陪伴")||!strcmp(s,"安静陪我一会儿")||!strcmp(s,"不要自己动"))cJSON_AddBoolToObject(patch,"chat_actions",false);
    else if(!strcmp(s,"开启聊天动作")||!strcmp(s,"恢复聊天动作"))cJSON_AddBoolToObject(patch,"chat_actions",true);
    else if(!strcmp(s,"大声一点")||!strcmp(s,"音量大一点"))cJSON_AddNumberToObject(patch,"volume",prefs.volume>90?100:prefs.volume+10);
    else if(!strcmp(s,"小声一点")||!strcmp(s,"音量小一点"))cJSON_AddNumberToObject(patch,"volume",prefs.volume<10?0:prefs.volume-10);
    else if(!strcmp(s,"静音"))cJSON_AddNumberToObject(patch,"volume",0);
    else if(!strcmp(s,"取消静音"))cJSON_AddNumberToObject(patch,"volume",35);
    else if(!strcmp(s,"默认动作慢速"))cJSON_AddStringToObject(patch,"default_speed","slow");
    else if(!strcmp(s,"默认动作快速"))cJSON_AddStringToObject(patch,"default_speed","fast");
    else if(!strcmp(s,"默认动作正常速度"))cJSON_AddStringToObject(patch,"default_speed","normal");
    else{
        const char *prefixes[]={"音量设为","音量设置为","把音量调到","音量调到","默认幅度设为","默认动作幅度设为"};
        recognized=false;
        for(unsigned k=0;k<sizeof(prefixes)/sizeof(prefixes[0]);++k){size_t len=strlen(prefixes[k]);
            if(!strncmp(s,prefixes[k],len)){int v;recognized=true;
                invalid=!percentage(s+len,&v)||(k>=4&&v<25);
                if(!invalid)cJSON_AddNumberToObject(patch,k>=4?"default_strength":"volume",v);
                break;}
        }
    }
    if(!recognized){cJSON_Delete(patch);return false;}
    esp_err_t err=invalid?ESP_ERR_INVALID_ARG:user_settings_update(patch);cJSON_Delete(patch);
    if(err){snprintf(reply,cap,err==ESP_ERR_INVALID_ARG?"设置未改变。音量范围零到一百，动作幅度范围二十五到一百。":"设置保存失败，原设置保持不变。");return true;}
    if(!strcmp(s,"关闭聊天动作")||!strcmp(s,"安静陪伴")||!strcmp(s,"安静陪我一会儿")||!strcmp(s,"不要自己动")||!strcmp(s,"开启聊天动作")||!strcmp(s,"恢复聊天动作")){snprintf(reply,cap,user_settings_chat_actions()?"已开启聊天动作，我会按聊天内容适当回应，也会安静待着。":"已关闭聊天动作，我会安静陪着你。明确动作口令仍然可用。");return true;}
    prefs=user_settings_get();snprintf(reply,cap,"已保存。音量百分之%d，默认动作%s，默认幅度百分之%d。",prefs.volume,
        prefs.default_speed==1?"慢速":prefs.default_speed==3?"快速":"正常速度",prefs.default_strength);
    return true;
}
