#pragma once
#include <stdbool.h>
typedef struct {
    unsigned frames,loud,quiet;
    int noise;
    bool speaking;
} voice_activity;
/* One frame is 20 ms. Learn the room level before looking for speech. */
static inline void voice_activity_feed(voice_activity *v,unsigned energy)
{
    ++v->frames;
    if(!v->speaking&&v->frames<=20){
        v->noise=(v->noise*(int)(v->frames-1)+(int)energy)/(int)v->frames;
        v->loud=0;return;
    }
    int threshold=v->noise*3;if(threshold<250)threshold=250;
    bool active=energy>(unsigned)threshold;
    if(!v->speaking){
        if(!active)v->noise=(v->noise*31+(int)energy)/32;
        v->loud=active?v->loud+1:0;
        if(v->loud>=3){v->speaking=true;v->quiet=0;}
    }else v->quiet=active?0:v->quiet+1;
}
