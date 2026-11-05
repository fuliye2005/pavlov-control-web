#pragma once
#include <stdbool.h>
/* Schematic R19=20k/R20=10k, nominal ADC reference=3.3V. Not calibrated. */
static inline unsigned dog_supply_millivolts(unsigned raw)
{return (raw*9900u+2047u)/4095u;}
static inline bool dog_supply_sample_valid(unsigned raw,unsigned mv,unsigned ready)
{return raw<=4095u&&ready<=1u&&mv==dog_supply_millivolts(raw);}
static inline bool dog_supply_literal(const char **text,const char *literal)
{
    while(*literal){if(**text!=*literal)return false;++*text;++literal;}return true;
}
static inline bool dog_supply_number(const char **text,unsigned limit,unsigned *value)
{
    unsigned result=0,digits=0;
    while(**text>='0'&&**text<='9'){
        unsigned digit=(unsigned)(**text-'0');
        if(++digits>5||digit>limit||result>(limit-digit)/10)return false;
        result=result*10+digit;++*text;
    }
    if(!digits)return false;
    *value=result;
    return true;
}
static inline bool dog_supply_parse_line(const char *text,unsigned *raw,unsigned *mv,unsigned *ready)
{
    return dog_supply_literal(&text,"SUPPLY raw=")&&dog_supply_number(&text,4095,raw)&&
        dog_supply_literal(&text," mv=")&&dog_supply_number(&text,9900,mv)&&
        dog_supply_literal(&text," ready=")&&dog_supply_number(&text,1,ready)&&
        dog_supply_literal(&text," calibrated=0")&&!*text&&dog_supply_sample_valid(*raw,*mv,*ready);
}
static inline const char *dog_supply_classification(unsigned mv)
{
    if(mv<6000u)return "source_unconfirmed";
    if(mv<7000u)return "estimated_low";
    if(mv<=8700u)return "voltage_available";
    return "out_of_expected_range";
}
