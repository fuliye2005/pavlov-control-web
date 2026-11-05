#include "user_profile.h"
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <assert.h>
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
static SemaphoreHandle_t guard;
static user_profile current={.version=1};
static unsigned revision;
static bool persisted;
static bool nickname_valid(const char *s)
{
    size_t bytes=strlen(s);if(bytes>72)return false;unsigned count=0;bool visible=false;
    for(size_t k=0;k<bytes;){
        unsigned char c=(unsigned char)s[k];uint32_t cp;size_t n;
        if(c<128){cp=c;n=1;}
        else if(c>=0xc2&&c<=0xdf){cp=c&31;n=2;}
        else if(c>=0xe0&&c<=0xef){cp=c&15;n=3;}
        else if(c>=0xf0&&c<=0xf4){cp=c&7;n=4;}
        else return false;
        if(k+n>bytes)return false;
        for(size_t j=1;j<n;++j){unsigned char q=(unsigned char)s[k+j];if((q&0xc0)!=0x80)return false;cp=(cp<<6)|(q&63);}
        if((n==2&&cp<0x80)||(n==3&&cp<0x800)||(n==4&&cp<0x10000)||cp>0x10ffff||(cp>=0xd800&&cp<=0xdfff))return false;
        bool allowed=(cp<128&&(isalnum((unsigned char)cp)||cp==' '||cp=='-'||cp=='_'))||
            (cp>=0x3400&&cp<=0x9fff)||(cp>=0xc0&&cp<=0x24f)||cp==0xb7;
        if(!allowed||++count>24)return false;
        if(cp!=' ')visible=true;
        k+=n;
    }
    return !bytes||visible;
}
void user_profile_init(void)
{
    guard=xSemaphoreCreateMutex();assert(guard);nvs_handle_t h;
    if(nvs_open("pavlov_profile",NVS_READONLY,&h)==ESP_OK){
        user_profile s={0};size_t n=sizeof(s);
        if(nvs_get_blob(h,"profile",&s,&n)==ESP_OK&&n==sizeof(s)&&s.version==1&&s.brief<=1&&
           memchr(s.nickname,0,sizeof(s.nickname))&&nickname_valid(s.nickname)){current=s;persisted=true;}
        nvs_close(h);
    }
}
user_profile user_profile_get(void)
{xSemaphoreTake(guard,portMAX_DELAY);user_profile s=current;xSemaphoreGive(guard);return s;}
unsigned user_profile_revision(void)
{xSemaphoreTake(guard,portMAX_DELAY);unsigned v=revision;xSemaphoreGive(guard);return v;}
cJSON *user_profile_json(void)
{
    xSemaphoreTake(guard,portMAX_DELAY);user_profile s=current;bool saved=persisted;unsigned v=revision;xSemaphoreGive(guard);
    cJSON *j=cJSON_CreateObject();cJSON_AddStringToObject(j,"nickname",s.nickname);
    cJSON_AddStringToObject(j,"reply_style",s.brief?"brief":"natural");
    cJSON_AddBoolToObject(j,"has_preferences",s.nickname[0]||s.brief);cJSON_AddBoolToObject(j,"persisted",saved);
    cJSON_AddNumberToObject(j,"revision",v);return j;
}
esp_err_t user_profile_update(cJSON *patch)
{
    if(!cJSON_IsObject(patch)||!patch->child)return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(guard,portMAX_DELAY);user_profile s=current;esp_err_t err=ESP_OK;
    cJSON *clear=cJSON_GetObjectItemCaseSensitive(patch,"clear");
    if(clear){
        if(patch->child!=clear||clear->next||!cJSON_IsTrue(clear)){err=ESP_ERR_INVALID_ARG;goto done;}
        memset(&s,0,sizeof(s));s.version=1;
    }else for(cJSON *p=patch->child;p;p=p->next){
        for(cJSON *q=p->next;q;q=q->next)if(p->string&&q->string&&!strcmp(p->string,q->string)){err=ESP_ERR_INVALID_ARG;goto done;}
        if(p->string&&!strcmp(p->string,"nickname")){
            if(!cJSON_IsString(p)||!nickname_valid(p->valuestring)){err=ESP_ERR_INVALID_ARG;goto done;}
            snprintf(s.nickname,sizeof(s.nickname),"%s",p->valuestring);
        }else if(p->string&&!strcmp(p->string,"reply_style")){
            if(!cJSON_IsString(p)||(strcmp(p->valuestring,"brief")&&strcmp(p->valuestring,"natural"))){err=ESP_ERR_INVALID_ARG;goto done;}
            s.brief=!strcmp(p->valuestring,"brief");
        }else{err=ESP_ERR_INVALID_ARG;goto done;}
    }
    if(!persisted||memcmp(&s,&current,sizeof(s))){nvs_handle_t h;err=nvs_open("pavlov_profile",NVS_READWRITE,&h);
        if(!err){err=nvs_set_blob(h,"profile",&s,sizeof(s));if(!err)err=nvs_commit(h);nvs_close(h);}
        if(!err){current=s;persisted=true;}}
    if(!err)++revision;
done:
    xSemaphoreGive(guard);return err;
}
bool user_profile_voice(const char *text,char *reply,size_t cap,bool *changed)
{
    bool question=strchr(text,'?')||strstr(text,"？");
    *changed=false;char b[1024];size_t n=0;
    while(*text&&n+1<sizeof(b)){
        if(!strncmp(text,"，",3)||!strncmp(text,"。",3)||!strncmp(text,"！",3)||!strncmp(text,"？",3)){text+=3;continue;}
        b[n++]=*text++;
    }
    b[n]=0;while(n&&(b[n-1]=='.'||b[n-1]=='?'||b[n-1]=='!'||isspace((unsigned char)b[n-1])))b[--n]=0;
    char *s=b;while(isspace((unsigned char)*s))++s;
    const char *prefix[]={"Pavlov","pavlov","巴甫洛夫","帕夫洛夫","请"};
    for(int round=0;round<3;++round){bool found=false;
        for(unsigned k=0;k<sizeof(prefix)/sizeof(prefix[0]);++k){size_t len=strlen(prefix[k]);if(!strncmp(s,prefix[k],len)){s+=len;while(isspace((unsigned char)*s))++s;found=true;break;}}
        if(!found)break;
    }
    user_profile p=user_profile_get();
    if(!strcmp(s,"你记得什么")||!strcmp(s,"报告偏好")||!strcmp(s,"查询偏好")){
        if(p.nickname[0])snprintf(reply,cap,"保存的称呼是%s，回复风格%s。",p.nickname,p.brief?"简短":"自然");
        else snprintf(reply,cap,"尚未保存称呼，回复风格%s。",p.brief?"简短":"自然");
        return true;
    }
    if(!strcmp(s,"我叫什么名字")||!strcmp(s,"你记得我叫什么")){
        snprintf(reply,cap,p.nickname[0]?"保存的称呼是%s。":"尚未保存你的称呼。",p.nickname);return true;
    }
    const char *discussion[]={"是什么意思","什么意思","如果","假设","只是举例","不要","不用","不必","不许"};
    if(question)return false;
    for(unsigned k=0;k<sizeof(discussion)/sizeof(discussion[0]);++k)if(strstr(s,discussion[k]))return false;
    cJSON *patch=cJSON_CreateObject();bool recognized=true,name_only=false;
    if(!strcmp(s,"忘记我的偏好")||!strcmp(s,"清除我的偏好"))cJSON_AddBoolToObject(patch,"clear",true);
    else if(!strcmp(s,"忘记我的称呼")){name_only=true;cJSON_AddStringToObject(patch,"nickname","");}
    else if(!strcmp(s,"回答简短一点")||!strcmp(s,"以后回答简短一点")||!strcmp(s,"回复简短一点"))cJSON_AddStringToObject(patch,"reply_style","brief");
    else if(!strcmp(s,"回答自然一点")||!strcmp(s,"以后回答自然一点")||!strcmp(s,"回复自然一点"))cJSON_AddStringToObject(patch,"reply_style","natural");
    else{
        const char *names[]={"记住叫我","记住我叫","叫我"};recognized=false;
        for(unsigned k=0;k<sizeof(names)/sizeof(names[0]);++k){size_t len=strlen(names[k]);if(!strncmp(s,names[k],len)){
            const char *value=s+len;while(isspace((unsigned char)*value))++value;
            cJSON_AddStringToObject(patch,"nickname",value);recognized=true;break;}}
    }
    if(!recognized){cJSON_Delete(patch);return false;}
    bool clear=cJSON_IsTrue(cJSON_GetObjectItem(patch,"clear"));
    cJSON *name=cJSON_GetObjectItem(patch,"nickname");
    esp_err_t err=name&&!name_only&&(!name->valuestring[0])?ESP_ERR_INVALID_ARG:user_profile_update(patch);cJSON_Delete(patch);
    if(err){snprintf(reply,cap,err==ESP_ERR_INVALID_ARG?"偏好未改变。称呼可用中文、字母和数字，最多二十四字。":"偏好保存失败，原偏好保持不变。");return true;}
    *changed=true;p=user_profile_get();
    if(clear)snprintf(reply,cap,"已清除称呼和回复偏好。");
    else if(name_only)snprintf(reply,cap,"已清除称呼，回复风格保持%s。",p.brief?"简短":"自然");
    else if(p.nickname[0])snprintf(reply,cap,"已保存称呼%s，回复风格%s。",p.nickname,p.brief?"简短":"自然");
    else snprintf(reply,cap,"已保存%s回复风格。",p.brief?"简短":"自然");
    return true;
}
