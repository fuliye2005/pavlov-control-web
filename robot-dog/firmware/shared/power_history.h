#pragma once
#include <stdbool.h>
#include <stdint.h>
#define DOG_POWER_HISTORY_COUNT 60
#define DOG_POWER_HISTORY_WINDOW_MS 600000u
typedef struct {uint32_t at;uint16_t millivolts;} dog_power_history_point;
typedef struct {dog_power_history_point points[DOG_POWER_HISTORY_COUNT];unsigned next,count;uint32_t last_at;} dog_power_history;
static inline const dog_power_history_point *dog_power_history_at(const dog_power_history *h,unsigned index)
{return &h->points[(h->next+DOG_POWER_HISTORY_COUNT-h->count+index)%DOG_POWER_HISTORY_COUNT];}
static inline void dog_power_history_prune(dog_power_history *h,uint32_t now)
{while(h->count&&(uint32_t)(now-dog_power_history_at(h,0)->at)>DOG_POWER_HISTORY_WINDOW_MS)--h->count;}
static inline void dog_power_history_add(dog_power_history *h,uint32_t now,unsigned millivolts)
{
    dog_power_history_prune(h,now);
    if(millivolts>16000||(h->count&&(uint32_t)(now-h->last_at)<10000))return;
    h->points[h->next]=(dog_power_history_point){now,(uint16_t)millivolts};h->next=(h->next+1)%DOG_POWER_HISTORY_COUNT;
    if(h->count<DOG_POWER_HISTORY_COUNT)++h->count;
    h->last_at=now;
}
/* Cosmetic reading-change classification only. Not a battery forecast or guard. */
static inline const char *dog_power_history_trend(const dog_power_history *h,bool fresh,bool battery,bool calibrated,uint32_t now,int *delta)
{
    *delta=0;
    if(!fresh||!h->count||(uint32_t)(now-dog_power_history_at(h,h->count-1)->at)>20000)return "unavailable";
    if(!battery)return "source_required";
    if(!calibrated)return "reference_required";
    for(unsigned i=0;i<h->count;i++){
        unsigned mv=dog_power_history_at(h,i)->millivolts;
        if(mv<5800||mv>9000)return "out_of_range";
    }
    if(h->count<12||(uint32_t)(dog_power_history_at(h,h->count-1)->at-dog_power_history_at(h,0)->at)<120000)return "collecting";
    int first=0,last=0;
    for(unsigned i=0;i<3;i++){first+=dog_power_history_at(h,i)->millivolts;last+=dog_power_history_at(h,h->count-3+i)->millivolts;}
    *delta=(last-first)/3;
    return *delta<=-100?"falling":*delta>=100?"rising":"stable";
}
