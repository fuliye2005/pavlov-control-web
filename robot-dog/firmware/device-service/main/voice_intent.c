#include "voice_intent.h"
#include "user_settings.h"
#include "../../shared/action_catalog.h"
#include <string.h>
#include <stdio.h>
#include <ctype.h>
void voice_intent_clear(voice_intent *i)
{user_preferences p=user_settings_get();memset(i,0,sizeof(*i));strcpy(i->action,"none");i->repeats=1;i->speed=p.default_speed;i->strength=p.default_strength;}
static bool number(cJSON *j,int min,int max,int *out)
{if(!cJSON_IsNumber(j)||j->valuedouble<min||j->valuedouble>max||(int)j->valuedouble!=j->valuedouble)return false;*out=(int)j->valuedouble;return true;}
bool voice_intent_parse(cJSON *j,voice_intent *i)
{
    voice_intent_clear(i);if(!cJSON_IsObject(j))return false;
    const char *fields[]={"action","reply","repeats","speed","strength"};
    for(cJSON *p=j->child;p;p=p->next){
        bool known=false;for(unsigned k=0;k<5;++k)if(p->string&&!strcmp(p->string,fields[k]))known=true;
        if(!known)return false;
        for(cJSON *q=p->next;q;q=q->next)if(q->string&&!strcmp(p->string,q->string))return false;
    }
    cJSON *a=cJSON_GetObjectItemCaseSensitive(j,"action"),*r=cJSON_GetObjectItemCaseSensitive(j,"reply");
    cJSON *s=cJSON_GetObjectItemCaseSensitive(j,"speed");
    if(!cJSON_IsString(a)||!cJSON_IsString(r)||(!r->valuestring[0]&&!strcmp(a->valuestring,"none"))||strlen(r->valuestring)>=sizeof(i->reply)||!cJSON_IsString(s))return false;
    if(strcmp(a->valuestring,"none")&&strcmp(a->valuestring,"stop")&&strcmp(a->valuestring,"status")&&strcmp(a->valuestring,"direction")&&strcmp(a->valuestring,"time")&&
       strcmp(a->valuestring,"greet")&&strcmp(a->valuestring,"show")&&!dog_action_by_name(a->valuestring))return false;
    if(!number(cJSON_GetObjectItemCaseSensitive(j,"repeats"),1,3,&i->repeats)||
       !number(cJSON_GetObjectItemCaseSensitive(j,"strength"),25,100,&i->strength))return false;
    i->speed=!strcmp(s->valuestring,"slow")?1:!strcmp(s->valuestring,"normal")?2:!strcmp(s->valuestring,"fast")?3:0;
    if(!i->speed)return false;
    snprintf(i->action,sizeof(i->action),"%s",a->valuestring);snprintf(i->reply,sizeof(i->reply),"%s",r->valuestring);return true;
}
static void normalized(const char *text,char *out,size_t cap)
{
    size_t n=0;
    while(*text&&n+1<cap){
        unsigned char c=(unsigned char)*text;
        if(c<128&&(isspace(c)||ispunct(c))){++text;continue;}
        if(!strncmp(text,"，",3)||!strncmp(text,"。",3)||!strncmp(text,"！",3)||!strncmp(text,"？",3)||!strncmp(text,"、",3)){text+=3;continue;}
        out[n++]=c<128?(char)tolower(c):(char)c;++text;
    }
    out[n]=0;
}
bool voice_intent_named(const char *text)
{char s[1024];normalized(text,s,sizeof(s));return strstr(s,"pavlov")||strstr(s,"巴甫洛夫")||strstr(s,"巴夫洛夫")||strstr(s,"帕夫洛夫");}
bool voice_intent_locomotion(const char *a)
{return !strcmp(a,"forward")||!strcmp(a,"backward")||!strcmp(a,"turn_left")||!strcmp(a,"turn_right");}
bool voice_intent_denied(const char *text)
{
    const char *words[]={"不要","别","不用","不许","不必","不想","是什么意思","什么意思","假设","如果","只是举例"};
    for(unsigned k=0;k<sizeof(words)/sizeof(words[0]);++k)if(strstr(text,words[k]))return true;
    return false;
}
const char *voice_intent_label(const char *a)
{
    static const char *names[][2]={{"jump","跳跃"},{"shake_leg","抖腿"},{"wag","摇尾巴"},{"handshake","握手"},{"stand","站立"},{"squat","蹲下"},
        {"lie_down","趴下"},{"stretch","伸懒腰"},{"forward","前进"},{"backward","后退"},{"turn_left","左转"},
        {"turn_right","右转"},{"rock_fb","前后摇摆"},{"rock_lr","左右摇摆"},{"step","原地踏步"},{"stop","停止"},
        {"greet","打招呼"},{"show","动作展示"},{"status","状态查询"},{"direction","方向查询"},{"time","机内时间查询"}};
    for(unsigned k=0;k<sizeof(names)/sizeof(names[0]);++k)if(!strcmp(a,names[k][0]))return names[k][1];
    return "聊天";
}
bool voice_intent_local(const char *text,voice_intent *i)
{
    voice_intent_clear(i);char buf[1024];normalized(text,buf,sizeof(buf));char *s=buf;
    const char *prefixes[]={"pavlov","巴甫洛夫","巴夫洛夫","帕夫洛夫","请","帮我","给我","麻烦你","麻烦","你","首先","先"};
    for(unsigned round=0;round<4;++round){bool found=false;
        for(unsigned k=0;k<sizeof(prefixes)/sizeof(prefixes[0]);++k){size_t len=strlen(prefixes[k]);if(!strncmp(s,prefixes[k],len)){s+=len;found=true;break;}}
        if(!found)break;
    }
    static const struct {const char *command,*reply;} help[]={
        {"偏好帮助","可以说记住叫我小陈，回答简短一点，回答自然一点，或你记得什么。说忘记我的偏好，可以清除称呼和回复风格。"},
        {"记忆帮助","可以说记住叫我小陈，回答简短一点，回答自然一点，或你记得什么。说忘记我的偏好，可以清除称呼和回复风格。"},
        {"模式帮助","可以说陪伴模式、动作模式或休息模式切换，说报告模式查询当前模式。休息时说巴甫洛夫醒醒恢复陪伴。休息后的语音唤醒也需要网络。"},
        {"帮助","我能陪你聊天，也会摇尾巴、握手和站立。说报告状态听联网和供电情况。说动作帮助听动作口令，说设置帮助听设置方法，模式帮助听切换方法，偏好帮助听记忆用法。"},
        {"你会什么","我能陪你聊天，也会摇尾巴、握手和站立。说报告状态听联网和供电情况。说动作帮助听动作口令，说设置帮助听设置方法，模式帮助听切换方法，偏好帮助听记忆用法。"},
        {"你能做什么","我能陪你聊天，也会摇尾巴、握手和站立。说报告状态听联网和供电情况。说动作帮助听动作口令，说设置帮助听设置方法，模式帮助听切换方法，偏好帮助听记忆用法。"},
        {"使用说明","我能陪你聊天，也会摇尾巴、握手和站立。说报告状态听联网和供电情况。说动作帮助听动作口令，说设置帮助听设置方法，模式帮助听切换方法，偏好帮助听记忆用法。"},
        {"教我怎么用","我能陪你聊天，也会摇尾巴、握手和站立。说报告状态听联网和供电情况。说动作帮助听动作口令，说设置帮助听设置方法，模式帮助听切换方法，偏好帮助听记忆用法。"},
        {"动作帮助","可以说：摇尾巴、握手、站起来、蹲下、趴下、打个招呼、展示一下。行走时说，巴甫洛夫，向前走。停止口令是，停止运动。"},
        {"设置帮助","可以说：音量设为百分之四十、大声一点、小声一点、默认动作慢速、默认幅度设为百分之五十，或报告设置。当前带线动作幅度上限百分之二十五。"}};
    for(unsigned k=0;k<sizeof(help)/sizeof(help[0]);++k)if(!strcmp(s,help[k].command)){
        i->local=true;snprintf(i->reply,sizeof(i->reply),"%s",help[k].reply);return true;
    }
    /* Modifiers must be followed by an exact command. Questions, negations and
       compound instructions continue through the strict model path. */
    for(unsigned round=0;round<2;++round){
        static const struct {const char *text;int speed,strength;} mods[]={
            {"慢速",1,0},{"慢慢",1,0},{"快速",3,0},{"正常速度",2,0},
            {"小幅度",0,25},{"小幅",0,25},{"轻轻",0,25},{"大幅度",0,100},{"大幅",0,100}};
        bool found=false;
        for(unsigned k=0;k<sizeof(mods)/sizeof(mods[0]);++k){size_t n=strlen(mods[k].text);
            if(!strncmp(s,mods[k].text,n)){s+=n;if(mods[k].speed)i->speed=mods[k].speed;
                if(mods[k].strength)i->strength=mods[k].strength;
                found=true;break;}}
        if(!found)break;
    }
    static const char *map[][2]={
        {"停","stop"},{"停下","stop"},{"停止","stop"},{"停下来","stop"},{"停止运动","stop"},
        {"别动","stop"},{"不要动","stop"},{"不要再动了","stop"},{"别走了","stop"},{"站住","stop"},
        {"摇尾巴","wag"},{"摇摇尾巴","wag"},{"摇一下尾巴","wag"},{"摇尾巴吧","wag"},
        {"跳跃","jump"},{"跳一下","jump"},{"跳一跳","jump"},{"抖腿","shake_leg"},{"抖一下腿","shake_leg"},{"抖抖腿","shake_leg"},{"握手","handshake"},{"握个手","handshake"},{"握一下手","handshake"},
        {"站立","stand"},{"站起来","stand"},{"站起来吧","stand"},{"站好","stand"},
        {"蹲下","squat"},{"坐下","squat"},{"趴下","lie_down"},{"躺下","lie_down"},
        {"打个招呼","greet"},{"打招呼","greet"},{"表演一下","show"},{"展示一下","show"},{"动作展示","show"},
        {"查询电量","status"},{"报告电量","status"},{"还有多少电","status"},{"电量多少","status"},{"供电电压","status"},{"查询电压","status"},{"电压多少","status"},
        {"报告状态","status"},{"查询状态","status"},{"现在什么状态","status"},{"网络状态","status"},
        {"报告方向","direction"},{"查询方向","direction"},{"方向查询","direction"},{"现在朝哪里","direction"},{"现在朝哪边","direction"},{"你朝哪里","direction"},{"查看指南针","direction"},
        {"报时","time"},{"时间查询","time"},{"告诉我现在几点","time"},{"告诉我几点了","time"},{"现在时间是多少","time"},{"报一下时间","time"},{"现在几点","time"},{"现在几点了","time"},{"现在是几点","time"},{"几点了","time"},{"报告时间","time"},{"查询时间","time"},{"今天几号","time"},{"今天星期几","time"},
        {"伸个懒腰","stretch"},{"伸懒腰","stretch"},{"原地踏步","step"},
        {"前进","forward"},{"向前走","forward"},{"往前走","forward"},{"走两步","forward"},
        {"后退","backward"},{"向后走","backward"},{"往后退","backward"},
        {"左转","turn_left"},{"向左转","turn_left"},{"右转","turn_right"},{"向右转","turn_right"}};
    for(unsigned k=0;k<sizeof(map)/sizeof(map[0]);++k){
        if(!strcmp(s,map[k][0])){
            snprintf(i->action,sizeof(i->action),"%s",map[k][1]);i->local=true;
            if(!strcmp(i->action,"wag"))i->repeats=2;
            i->reply[0]=0;return true;
        }
    }
    return false;
}
cJSON *voice_intent_json(const voice_intent *i)
{
    cJSON *j=cJSON_CreateObject();cJSON_AddStringToObject(j,"action",i->action);cJSON_AddStringToObject(j,"reply",i->reply);
    cJSON_AddNumberToObject(j,"repeats",i->repeats);cJSON_AddStringToObject(j,"speed",i->speed==1?"slow":i->speed==3?"fast":"normal");
    cJSON_AddNumberToObject(j,"strength",i->strength);cJSON_AddBoolToObject(j,"local",i->local);cJSON_AddBoolToObject(j,"expression",i->expression);return j;
}
void voice_reply_no_task_prompt(char *reply,size_t capacity)
{
    const char *prompts[]={"你想做","你想让我","你想要我","你需要我","需要我","要我帮","你希望我", 
        "有什么需要","有什么我可以","你想聊","想聊什么","聊些什么","要不要我","今天想做","想做点什么"};
    char kept[401];size_t used=0;const char *start=reply,*p=reply;
    while(*p){
        size_t end=(!strncmp(p,"。",3)||!strncmp(p,"！",3)||!strncmp(p,"？",3))?3:
            (*p=='.'||*p=='!'||*p=='?'||*p=='\n')?1:0;
        if(!end){++p;if(*p)continue;}
        else p+=end;
        size_t len=(size_t)(p-start);char sentence[401];
        if(len>=sizeof(sentence))len=sizeof(sentence)-1;
        memcpy(sentence,start,len);sentence[len]=0;bool invitation=false;
        for(unsigned k=0;k<sizeof(prompts)/sizeof(prompts[0]);++k)if(strstr(sentence,prompts[k]))invitation=true;
        if(!invitation&&used+len<sizeof(kept)){memcpy(kept+used,sentence,len);used+=len;}
        start=p;
    }
    kept[used]=0;snprintf(reply,capacity,"%s",used?kept:"我在。");
}
