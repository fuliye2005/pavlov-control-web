#pragma once
#include <stdbool.h>
#include <stdint.h>
/* No default battery inference. Thresholds are advisory only and require an
 * explicit 2S source and independently measured reference at the sampled node. */
typedef enum {DOG_POWER_UNKNOWN,DOG_POWER_USB,DOG_POWER_BATTERY_2S} dog_power_source;
typedef struct {uint32_t version;dog_power_source source;unsigned reference_raw,reference_mv,low_mv,recover_mv,alerts_enabled;} dog_power_config;
typedef struct {bool low,pending,have_notification;uint32_t pending_at,last_notification;} dog_power_monitor;
static inline bool dog_power_config_valid(const dog_power_config *c)
{
    if(c->version!=1||c->source>DOG_POWER_BATTERY_2S||c->alerts_enabled>1||c->low_mv<6400||c->low_mv>7400||c->recover_mv<c->low_mv+150||c->recover_mv>7800)return false;
    if(c->alerts_enabled&&c->source!=DOG_POWER_BATTERY_2S)return false;
    if(!c->reference_raw&&!c->reference_mv)return !c->alerts_enabled;
    if(c->reference_raw<2000||c->reference_raw>4095||c->reference_mv<6000||c->reference_mv>9000)return false;
    unsigned nominal=(c->reference_raw*9900u+2047)/4095;
    return c->reference_mv*10u>=nominal*8u&&c->reference_mv*10u<=nominal*12u;
}
static inline bool dog_power_calibrated(const dog_power_config *c)
{return dog_power_config_valid(c)&&c->reference_raw&&c->reference_mv;}
static inline unsigned dog_power_mv(const dog_power_config *c,unsigned raw)
{return raw<=4095&&dog_power_calibrated(c)?(raw*c->reference_mv+c->reference_raw/2)/c->reference_raw:0;}
static inline bool dog_power_update(dog_power_monitor *s,const dog_power_config *c,bool fresh,unsigned raw,uint32_t now)
{
    unsigned mv=dog_power_mv(c,raw);
    if(!fresh||!c->alerts_enabled||c->source!=DOG_POWER_BATTERY_2S||!mv||mv<5800||mv>9000){s->low=false;s->pending=false;return false;}
    bool condition=s->low?mv<c->recover_mv:mv<=c->low_mv;
    if(!condition){s->low=false;s->pending=false;return false;}
    if(!s->pending){s->pending=true;s->pending_at=now;}
    if(!s->low&&(uint32_t)(now-s->pending_at)>=10000)s->low=true;
    if(s->low&&(!s->have_notification||(uint32_t)(now-s->last_notification)>=300000)){
        s->have_notification=true;s->last_notification=now;return true;
    }
    return false;
}
