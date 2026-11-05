#include "voice_sequence.h"
#include "../../shared/action_catalog.h"
#include <string.h>
#include <ctype.h>
static unsigned separator(const char *s,bool *word)
{
    const char *words[]={"然后","接着","之后","最后","再"};
    for(unsigned i=0;i<sizeof(words)/sizeof(words[0]);++i)
        if(!strncmp(s,words[i],strlen(words[i]))){*word=true;return strlen(words[i]);}
    const char *punct[]={"，","、","；","。",",",";"};
    for(unsigned i=0;i<sizeof(punct)/sizeof(punct[0]);++i)
        if(!strncmp(s,punct[i],strlen(punct[i]))){*word=false;return strlen(punct[i]);}
    return 0;
}
int voice_sequence_parse(const char *text,voice_sequence *out)
{
    memset(out,0,sizeof(*out));
    if(!text||strlen(text)>1023)return VOICE_SEQUENCE_INVALID;
    voice_intent single;
    if(voice_intent_local(text,&single))return VOICE_SEQUENCE_NONE;
    const char *p=text;while(*p&&isspace((unsigned char)*p))++p;
    const char *names[]={"巴甫洛夫","巴夫洛夫","帕夫洛夫","pavlov","Pavlov"};
    for(unsigned i=0;i<sizeof(names)/sizeof(names[0]);++i){unsigned n=strlen(names[i]);if(!strncmp(p,names[i],n)){p+=n;break;}}
    while(*p){bool word=false;unsigned n=separator(p,&word);if(n&&!word){p+=n;continue;}if(isspace((unsigned char)*p)){++p;continue;}break;}
    if(!strncmp(p,"先",strlen("先")))p+=strlen("先");
    char fragment[256];unsigned used=0,segments=0,recognized=0;bool saw_separator=false,invalid=false,dangling=false;
    for(;;){
        if(*p&&isspace((unsigned char)*p)){++p;continue;}
        bool word=false;unsigned n=*p?separator(p,&word):0;
        if(!*p||n){
            if(used){
                fragment[used]=0;voice_intent item;
                bool local=voice_intent_local(fragment,&item);
                const dog_action *a=local?dog_action_by_name(item.action):NULL;
                bool stop=local&&!strcmp(item.action,"stop");
                if(a||stop){
                    ++recognized;
                    if(out->count<VOICE_SEQUENCE_MAX){out->steps[out->count++]=item;if(a)out->duration_ms+=dog_action_duration(a,item.repeats,item.speed);}
                    else invalid=true;
                }else invalid=true;
                ++segments;used=0;dangling=false;
            }
            if(!*p)break;
            /* A terminal ASR full stop is punctuation, not a second command. */
            if(word||p[n])saw_separator=true;
            if(word)dangling=true;
            p+=n;continue;
        }
        if(used+1>=sizeof(fragment))return VOICE_SEQUENCE_INVALID;
        fragment[used++]=*p++;
    }
    if(!saw_separator||!recognized)return VOICE_SEQUENCE_NONE;
    if(invalid||dangling||segments<2||out->count<2||out->duration_ms>VOICE_SEQUENCE_MAX_MS||voice_intent_denied(text))return VOICE_SEQUENCE_INVALID;
    for(unsigned i=0;i+1<out->count;++i)if(!strcmp(out->steps[i].action,"stop"))return VOICE_SEQUENCE_INVALID;
    return VOICE_SEQUENCE_VALID;
}
