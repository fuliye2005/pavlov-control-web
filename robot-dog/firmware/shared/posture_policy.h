#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
typedef enum {DOG_POSTURE_UNKNOWN,DOG_POSTURE_UPRIGHT,DOG_POSTURE_TILTED,DOG_POSTURE_SIDE,DOG_POSTURE_INVERTED} dog_posture;
typedef struct {dog_posture state,candidate;unsigned count;uint32_t sequence,since;} dog_posture_tracker;
static inline const char *dog_posture_name(dog_posture s)
{return s==DOG_POSTURE_UPRIGHT?"upright":s==DOG_POSTURE_TILTED?"tilted":s==DOG_POSTURE_SIDE?"side":s==DOG_POSTURE_INVERTED?"inverted":"unknown";}
static inline void dog_posture_update(dog_posture_tracker *t,bool valid,float vertical,uint32_t sequence,uint32_t now)
{
    if(!valid||!isfinite(vertical)||fabsf(vertical)>1.001f){t->state=t->candidate=DOG_POSTURE_UNKNOWN;t->count=0;return;}
    if(sequence==t->sequence)return;
    t->sequence=sequence;
    dog_posture next=vertical>=.94f?DOG_POSTURE_UPRIGHT:vertical<=-.85f?DOG_POSTURE_INVERTED:fabsf(vertical)<=.35f?DOG_POSTURE_SIDE:DOG_POSTURE_TILTED;
    /* Hysteresis keeps sensor noise near the boundary from flashing warnings. */
    if(t->state==DOG_POSTURE_UPRIGHT&&vertical>=.86f)next=DOG_POSTURE_UPRIGHT;
    if(t->state==DOG_POSTURE_INVERTED&&vertical<=-.70f)next=DOG_POSTURE_INVERTED;
    if(t->state==DOG_POSTURE_SIDE&&fabsf(vertical)<=.50f)next=DOG_POSTURE_SIDE;
    if(next!=t->candidate){t->candidate=next;t->since=now;t->count=1;}
    else if(t->count<3)++t->count;
    if(t->count>=3&&(uint32_t)(now-t->since)>=750)t->state=next;
}
