#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "supply_profile.h"
typedef struct {unsigned id,address,ready,error,range,status;uint32_t sequence,age;int axis[6],temperature;} dog_sensor_sample;
static inline bool dog_sensor_unsigned(const char **text,uint32_t limit,uint32_t *value)
{
    uint32_t n=0;unsigned digits=0;
    while(**text>='0'&&**text<='9'){
        unsigned d=(unsigned)(**text-'0');
        if(++digits>10||d>limit||n>(limit-d)/10)return false;
        n=n*10+d;++*text;
    }
    if(!digits)return false;
    *value=n;return true;
}
static inline bool dog_sensor_word(const char **text,int *value)
{
    bool negative=**text=='-';if(negative)++*text;uint32_t n;
    if(!dog_sensor_unsigned(text,negative?32768:32767,&n))return false;
    *value=negative?-(int)n:(int)n;return true;
}
static inline bool dog_sensor_field(const char **text,const char *name,uint32_t max,unsigned *value)
{uint32_t n;if(!dog_supply_literal(text,name)||!dog_sensor_unsigned(text,max,&n))return false;*value=(unsigned)n;return true;}
static inline bool dog_sensor_parse(const char *text,bool imu,dog_sensor_sample *s)
{
    if(!dog_supply_literal(&text,imu?"IMU id=":"MAG id=")||!dog_sensor_field(&text,"",255,&s->id)||
        !dog_sensor_field(&text," addr=",127,&s->address)||!dog_sensor_field(&text," ready=",1,&s->ready)||
        !dog_supply_literal(&text," seq=")||!dog_sensor_unsigned(&text,UINT32_MAX,&s->sequence)||
        !dog_supply_literal(&text," age=")||!dog_sensor_unsigned(&text,UINT32_MAX,&s->age))return false;
    const char *im[]={" ax="," ay="," az="," gx="," gy="," gz="},*ma[]={" x="," y="," z="};
    for(unsigned i=0;i<(imu?6u:3u);i++)if(!dog_supply_literal(&text,imu?im[i]:ma[i])||!dog_sensor_word(&text,&s->axis[i]))return false;
    if(imu){if(!dog_supply_literal(&text," temp=")||!dog_sensor_word(&text,&s->temperature))return false;}
    else if(!dog_sensor_field(&text," range=",32,&s->range)||(s->range!=8&&s->range!=16&&s->range!=32)||!dog_sensor_field(&text," status=",255,&s->status))return false;
    if(!dog_sensor_field(&text," err=",4,&s->error)||*text)return false;
    return !s->ready||(s->id==(imu?5u:144u)&&(imu?(s->address==106||s->address==107):s->address==124)&&s->sequence&&s->age<1500&&!s->error&& (imu||!(s->status&2)));
}
static inline bool dog_reference_parse(const char *text,unsigned *raw,unsigned *ready)
{return dog_supply_literal(&text,"ADCREF raw=")&&dog_supply_number(&text,4095,raw)&&dog_supply_literal(&text," ready=")&&dog_supply_number(&text,1,ready)&&!*text;}
