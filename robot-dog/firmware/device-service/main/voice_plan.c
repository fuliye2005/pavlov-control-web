#include "voice_plan.h"
#include "../../shared/action_catalog.h"
#include <string.h>
static bool fields(cJSON *object,const char **allowed,unsigned count)
{
    if(!cJSON_IsObject(object))return false;
    for(cJSON *p=object->child;p;p=p->next){
        bool known=false;
        for(unsigned i=0;i<count;++i)if(p->string&&!strcmp(p->string,allowed[i]))known=true;
        if(!known)return false;
        for(cJSON *q=p->next;q;q=q->next)if(q->string&&!strcmp(p->string,q->string))return false;
    }
    return true;
}
bool voice_plan_parse(cJSON *json,voice_intent *intent,voice_sequence *sequence)
{
    memset(sequence,0,sizeof(*sequence));
    cJSON *action=cJSON_GetObjectItemCaseSensitive(json,"action");
    if(!cJSON_IsString(action)||strcmp(action->valuestring,"sequence"))return voice_intent_parse(json,intent);
    const char *outer[]={"action","reply","repeats","speed","strength","steps"};
    if(!fields(json,outer,6))return false;
    cJSON *steps=cJSON_GetObjectItemCaseSensitive(json,"steps");
    int n=cJSON_GetArraySize(steps);
    if(!cJSON_IsArray(steps)||n<2||n>VOICE_SEQUENCE_MAX)return false;
    cJSON *single=cJSON_Duplicate(json,true);
    if(!single)return false;
    cJSON_DeleteItemFromObjectCaseSensitive(single,"steps");
    cJSON_ReplaceItemInObjectCaseSensitive(single,"action",cJSON_CreateString("stand"));
    bool valid=voice_intent_parse(single,intent)&&intent->repeats==1;
    cJSON_Delete(single);if(!valid)return false;
    const char *inner[]={"action","repeats","speed","strength"};
    for(int i=0;i<n;++i){
        cJSON *step=cJSON_GetArrayItem(steps,i);
        if(!fields(step,inner,4))return false;
        single=cJSON_Duplicate(step,true);if(!single)return false;
        cJSON_AddStringToObject(single,"reply","");
        voice_intent item;valid=voice_intent_parse(single,&item);cJSON_Delete(single);
        if(!valid)return false;
        const dog_action *a=dog_action_by_name(item.action);
        bool stop=!strcmp(item.action,"stop");
        if(!a&&!(stop&&i==n-1&&item.repeats==1))return false;
        if(a)sequence->duration_ms+=dog_action_duration(a,item.repeats,item.speed);
        if(sequence->duration_ms>VOICE_SEQUENCE_MAX_MS)return false;
        sequence->steps[sequence->count++]=item;
    }
    strcpy(intent->action,"sequence");intent->expression=false;return true;
}
