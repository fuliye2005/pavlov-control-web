#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
typedef struct {unsigned year,month,day,hour,minute,second,weekday;} dog_rtc_date;
static inline unsigned dog_rtc_month_days(unsigned year,unsigned month)
{
    static const uint8_t days[]={31,28,31,30,31,30,31,31,30,31,30,31};
    return month<1||month>12?0:days[month-1]+(month==2&&year%4==0);
}
static inline bool dog_rtc_date_valid(const dog_rtc_date *d)
{return d->year>=2000&&d->year<=2099&&d->month>=1&&d->month<=12&&d->day>=1&&d->day<=dog_rtc_month_days(d->year,d->month)&&d->hour<24&&d->minute<60&&d->second<60&&d->weekday<7;}
static inline unsigned dog_rtc_weekday(const dog_rtc_date *d)
{
    unsigned days=0;for(unsigned y=2000;y<d->year;y++)days+=365+(y%4==0);
    for(unsigned m=1;m<d->month;m++)days+=dog_rtc_month_days(d->year,m);
    return (6+days+d->day-1)%7;
}
static inline uint32_t dog_rtc_seconds(const dog_rtc_date *d)
{
    uint32_t days=0;for(unsigned y=2000;y<d->year;y++)days+=365+(y%4==0);
    for(unsigned m=1;m<d->month;m++)days+=dog_rtc_month_days(d->year,m);
    return ((days+d->day-1)*24+d->hour)*3600+d->minute*60+d->second;
}
static inline bool dog_rtc_from_seconds(uint32_t seconds,dog_rtc_date *d)
{
    if(seconds>=3155760000u)return false;
    unsigned days=seconds/86400u;d->weekday=(6+days)%7;
    d->hour=seconds%86400u/3600u;d->minute=seconds%3600u/60u;d->second=seconds%60u;
    d->year=2000;while(days>=365u+(d->year%4==0)){days-=365u+(d->year%4==0);++d->year;}
    d->month=1;while(days>=dog_rtc_month_days(d->year,d->month)){days-=dog_rtc_month_days(d->year,d->month);++d->month;}
    d->day=days+1;return true;
}
static inline unsigned dog_rtc_bcd(unsigned v){return (v/10)*16+v%10;}
static inline bool dog_rtc_unbcd(uint8_t b,unsigned *v)
{if((b&15)>9||(b>>4)>9)return false;*v=(b>>4)*10+(b&15);return true;}
static inline bool dog_rtc_decode(uint8_t control,const uint8_t *r,dog_rtc_date *d)
{
    if((control&0xa0)||(r[0]&0x80)||(r[1]&0x80)||(r[2]&0xc0)||(r[3]&0xc0)||(r[4]&0xf8)||(r[5]&0xe0))return false;
    unsigned year;
    if(!dog_rtc_unbcd(r[0],&d->second)||!dog_rtc_unbcd(r[1],&d->minute)||!dog_rtc_unbcd(r[2]&((control&2)?0x1f:0x3f),&d->hour)||
       !dog_rtc_unbcd(r[3],&d->day)||!dog_rtc_unbcd(r[5],&d->month)||!dog_rtc_unbcd(r[6],&year))return false;
    if(control&2){if(d->hour<1||d->hour>12)return false;d->hour=d->hour%12+((r[2]&0x20)?12:0);}
    d->year=2000+year;d->weekday=r[4];return dog_rtc_date_valid(d)&&dog_rtc_weekday(d)==d->weekday;
}
static inline void dog_rtc_encode(const dog_rtc_date *d,uint8_t *r)
{r[0]=dog_rtc_bcd(d->second);r[1]=dog_rtc_bcd(d->minute);r[2]=dog_rtc_bcd(d->hour);r[3]=dog_rtc_bcd(d->day);r[4]=dog_rtc_weekday(d);r[5]=dog_rtc_bcd(d->month);r[6]=dog_rtc_bcd(d->year-2000);}
static inline bool dog_rtc_parse_iso(const char *s,size_t len,dog_rtc_date *d)
{
    if(len!=20||s[4]!='-'||s[7]!='-'||s[10]!='T'||s[13]!=':'||s[16]!=':'||s[19]!='Z')return false;
    const unsigned starts[]={0,5,8,11,14,17},counts[]={4,2,2,2,2,2};unsigned values[6]={0};
    for(unsigned f=0;f<6;f++)for(unsigned i=0;i<counts[f];i++){char c=s[starts[f]+i];if(c<'0'||c>'9')return false;values[f]=values[f]*10+(unsigned)(c-'0');}
    *d=(dog_rtc_date){values[0],values[1],values[2],values[3],values[4],values[5],0};
    if(!dog_rtc_date_valid(d))return false;
    d->weekday=dog_rtc_weekday(d);return true;
}
