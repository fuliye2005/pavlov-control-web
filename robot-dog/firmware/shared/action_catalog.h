#pragma once
#include <stdint.h>

/* DOG-001 factory raw timer channels: offsets are tail, TIM3 CH1..CH4.
 * Source: vendor 04.01 motor_control.c, with DOG-001 adaptations below.
 * Maximum leg envelope is 450us; handshake uses the observed 250us range.
 * All actions have a gradual baseline pre/postlude.
 * Names describe requested behavior, not sensor-verified mechanical results. */
typedef struct {
    int16_t offset[5];
    uint16_t transition_ms, hold_ms;
} dog_action_frame;
typedef struct {
    const char *name;
    const dog_action_frame *frames;
    uint8_t id, count;
} dog_action;
#define DOG_ACTION_MAX_MS 12000u
#define DOG_ACTION_MAX_STRENGTH 100
#define DOG_ACTION_DEFAULT_STRENGTH 25
#define DOG_ACTION_SETTLE_MS 500u

static const dog_action_frame dog_backward[] = {
    {{-300,300,0,-300,0},100,50}, {{300,250,-150,-250,150},100,50},
    {{-300,0,-300,0,300},100,50}, {{300,0,300,0,-300},100,50},
    {{-300,-250,150,250,-150},100,50}
};
static const dog_action_frame dog_left[] = {
    /* Reverse the verified right-turn cycle, rather than negating angles.
     * The latter also turned right on DOG-001. Physical left still needs
     * acceptance; no timer remap or global channel swap is implied. */
    {{-300,150,0,150,0},100,50}, {{300,300,-300,300,-300},100,75},
    {{-300,0,-150,0,-150},100,50}
};
static const dog_action_frame dog_right[] = {
    {{-300,0,-150,0,-150},100,50}, {{300,300,-300,300,-300},100,75},
    {{-300,150,0,150,0},100,50}
};
static const dog_action_frame dog_stand[] = {{{0,0,0,0,0},200,300}};
static const dog_action_frame dog_squat[] = {
    {{-300,-450,0,0,450},300,100}, {{300,-450,0,0,450},200,400}
};
static const dog_action_frame dog_lie[] = {
    {{-300,0,-450,450,0},300,100}, {{300,0,-450,450,0},200,400}
};
static const dog_action_frame dog_stretch[] = {
    {{-300,150,-150,150,-150},200,100}, {{300,300,-300,300,-300},200,150},
    {{-300,450,-450,450,-450},200,250}, {{300,300,-300,300,-300},200,100},
    {{0,0,0,0,0},200,100}
};
static const dog_action_frame dog_handshake[] = {
    /* CH3 is the observed front-right leg. Keep the tail still, approach
     * within the working +250us diagnostic envelope, dwell, then return.
     * Preserve all speed/repeat durations used by existing ESP clients. */
    {{0,0,0,150,0},300,100}, {{0,0,0,250,0},200,400},
    {{0,0,0,150,0},200,0}, {{0,0,0,0,0},300,0}
};
static const dog_action_frame dog_rock_fb[] = {
    {{-300,-150,-150,150,150},200,200}, {{300,150,150,-150,-150},200,200}
};
static const dog_action_frame dog_rock_lr[] = {
    {{-300,-300,-300,-300,-300},200,300}, {{300,300,300,300,300},200,300}
};
static const dog_action_frame dog_step[] = {
    {{-100,-150,0,150,0},100,50}, {{0,0,0,0,0},100,25},
    {{100,0,150,0,-150},100,50}, {{0,0,0,0,0},100,25}
};
static const dog_action_frame dog_wag[] = {
    {{-300,0,0,0,0},200,200}, {{300,0,0,0,0},200,200}
};
static const dog_action_frame dog_forward[] = {
    {{-300,-300,0,300,0},100,50}, {{300,-250,150,250,-150},100,50},
    {{-300,0,300,0,-300},100,50}, {{300,0,-300,0,300},100,50},
    {{-300,250,-150,-250,150},100,50}
};
/* Vendor jump/shake structure adapted to DOG-001's existing 450us envelope.
 * Not a guarantee of airborne motion. Tail remains still for these candidates.
 * Vendor shake's 950us extreme is proportionally reduced, not copied. */
static const dog_action_frame dog_jump[] = {
    {{0,-450,0,0,450},200,700}, {{0,400,0,0,-400},200,300},
    {{0,0,0,0,0},200,100}
};
static const dog_action_frame dog_shake_leg[] = {
    {{0,380,-140,140,-380},200,50}, {{0,310,-140,140,-450},100,50},
    {{0,380,-140,140,-380},100,50}, {{0,310,-140,140,-450},100,50},
    {{0,380,-140,140,-380},100,50}, {{0,310,-140,140,-450},100,50},
    {{0,380,-140,140,-380},100,50}, {{0,310,-140,140,-450},100,50},
    {{0,0,0,0,0},200,100}
};
#define DOG_ENTRY(n,f,i) {n,f,i,sizeof(f)/sizeof((f)[0])}
static const dog_action dog_actions[] = {
    DOG_ENTRY("backward",dog_backward,1), DOG_ENTRY("turn_left",dog_left,2),
    DOG_ENTRY("turn_right",dog_right,3), DOG_ENTRY("stand",dog_stand,4),
    DOG_ENTRY("squat",dog_squat,5), DOG_ENTRY("lie_down",dog_lie,6),
    DOG_ENTRY("stretch",dog_stretch,7), DOG_ENTRY("handshake",dog_handshake,8),
    DOG_ENTRY("rock_fb",dog_rock_fb,9), DOG_ENTRY("rock_lr",dog_rock_lr,10),
    DOG_ENTRY("step",dog_step,11), DOG_ENTRY("wag",dog_wag,12),
    DOG_ENTRY("forward",dog_forward,13),
    DOG_ENTRY("jump",dog_jump,14), DOG_ENTRY("shake_leg",dog_shake_leg,15)
};
#define DOG_ACTION_COUNT (sizeof(dog_actions)/sizeof(dog_actions[0]))
static inline const dog_action *dog_action_by_id(unsigned id)
{
    for(unsigned i=0;i<DOG_ACTION_COUNT;++i)if(dog_actions[i].id==id)return &dog_actions[i];
    return 0;
}
static inline const dog_action *dog_action_by_name(const char *name)
{
    for(unsigned i=0;i<DOG_ACTION_COUNT;++i){
        const char *a=name,*b=dog_actions[i].name;
        while(*a&&*a==*b){++a;++b;}
        if(*a==*b)return &dog_actions[i];
    }
    return 0;
}
/* speed 1=slow, 2=normal, 3=fast. Align durations with the 10ms scheduler. */
static inline unsigned dog_action_time(unsigned ms,unsigned speed,unsigned minimum)
{
    unsigned scaled=speed==1?ms*2:speed==3?ms*3/4:ms;
    if(scaled<minimum)scaled=minimum;
    return (scaled+9)/10*10;
}
static inline unsigned dog_action_duration(const dog_action *a,unsigned repeats,unsigned speed)
{
    unsigned cycle=0;
    for(unsigned i=0;i<a->count;++i)
        cycle+=dog_action_time(a->frames[i].transition_ms,speed,100)+dog_action_time(a->frames[i].hold_ms,speed,0);
    return 2*DOG_ACTION_SETTLE_MS+cycle*repeats;
}
