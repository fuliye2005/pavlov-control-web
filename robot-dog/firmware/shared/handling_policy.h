#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <math.h>

/* A cosmetic handling hint, not a held/freefall classifier or motion guard.
 * Thresholds are provisional until whole-device physical acceptance. */
typedef struct {
    uint32_t sequence,last_at,since,blocked_until,hint_until,cooldown_until;
    unsigned count;
} dog_handling_tracker;

static inline bool dog_handling_pending(uint32_t now,uint32_t until)
{return until&&(int32_t)(until-now)>0;}

static inline void dog_handling_block(dog_handling_tracker *t,uint32_t now)
{
    t->count=0;t->hint_until=0;t->blocked_until=now+2000;
}

static inline bool dog_handling_hint(const dog_handling_tracker *t,uint32_t now)
{return !dog_handling_pending(now,t->blocked_until)&&dog_handling_pending(now,t->hint_until);}

static inline void dog_handling_update(dog_handling_tracker *t,const float *a,const float *gyro,uint32_t sequence,uint32_t now)
{
    if(sequence==t->sequence)return;
    t->sequence=sequence;
    if(t->last_at&&(uint32_t)(now-t->last_at)>900)t->count=0;
    t->last_at=now;
    float norm=0,turn=0;
    for(unsigned i=0;i<3;i++){
        if(!isfinite(a[i])||!isfinite(gyro[i])){dog_handling_block(t,now);return;}
        norm+=a[i]*a[i];turn+=gyro[i]*gyro[i];
    }
    norm=sqrtf(norm);turn=sqrtf(turn);
    /* Extreme values are not interpreted as playful handling. */
    if(norm<.65f||norm>1.35f||turn>220.f){dog_handling_block(t,now);return;}
    if(dog_handling_pending(now,t->blocked_until)||dog_handling_pending(now,t->cooldown_until)){t->count=0;return;}
    if(turn<25.f&&fabsf(norm-1.f)<.15f){t->count=0;return;}
    if(!t->count){t->since=now;t->count=1;}
    else if(t->count<3)++t->count;
    if(t->count>=3&&(uint32_t)(now-t->since)>=500){
        t->hint_until=now+2200;t->cooldown_until=now+8000;t->count=0;
    }
}
