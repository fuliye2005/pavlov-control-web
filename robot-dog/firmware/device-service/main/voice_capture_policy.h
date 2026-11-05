#pragma once
#include <stddef.h>
#include "voice_activity.h"
#define VOICE_WAIT_MS 15000
#define VOICE_MAX_MS 15000
#define VOICE_MAX_BYTES 512000u
#define VOICE_PREROLL_BYTES 16000u
#define VOICE_END_QUIET_FRAMES 75u
#define VOICE_TRAILING_KEEP_FRAMES 15u
static inline voice_activity voice_activity_resume(int noise,bool ready)
{
    voice_activity v={0};
    if(ready){v.noise=noise;v.frames=20;}
    return v;
}
static inline bool voice_capture_complete(const voice_activity *v,size_t used)
{return v->speaking&&v->quiet>=VOICE_END_QUIET_FRAMES&&used>=12800;}
static inline size_t voice_capture_trim_tail(size_t used,unsigned quiet)
{
    size_t trim=quiet>VOICE_TRAILING_KEEP_FRAMES?(size_t)(quiet-VOICE_TRAILING_KEEP_FRAMES)*640:0;
    return trim<used&&used-trim>=6400?used-trim:used;
}
