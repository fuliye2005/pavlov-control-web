#include "face_ui.h"
#include <stdlib.h>
static bool before(uint32_t now,uint32_t until){return until&&(int32_t)(until-now)>0;}
bool face_ui_peeking(const face_ui *u,uint32_t now){return before(now,u->peek_until);}
bool face_ui_patted(const face_ui *u,uint32_t now){return before(now,u->pat_until);}
face_ui_action face_ui_update(face_ui *u,bool pressed,int x,int y,uint32_t now)
{
    if(u->menu&&now-u->menu_at>8000&&!u->down)u->menu=false;
    if(pressed){
        if(!u->down){u->down=true;u->began=now;u->x=x;u->y=y;u->long_fired=false;u->moved=false;}
        if(abs(x-u->x)>14||abs(y-u->y)>14)u->moved=true;
        if(!u->menu&&!u->moved&&!u->long_fired&&now-u->began>=800){
            u->menu=true;u->menu_at=now;u->long_fired=true;++u->long_presses;}
        return FACE_UI_NONE;
    }
    if(!u->down)return FACE_UI_NONE;
    u->down=false;
    if(u->moved||u->long_fired||now-u->began<30)return FACE_UI_NONE;
    x=u->x;y=u->y;
    if(u->menu){
        u->menu_at=now;
        if(x<20||x>=300){u->menu=false;return FACE_UI_CLOSE;}
        if(y>=48&&y<100)return x<160?FACE_UI_LISTEN:FACE_UI_REST;
        if(y>=108&&y<160)return x<160?FACE_UI_QUIETER:FACE_UI_LOUDER;
        if(y>=168&&y<222){u->menu=false;return x<160?FACE_UI_PAIR:FACE_UI_CLOSE;}
        return FACE_UI_NONE;
    }
    if(u->x>=252&&u->y<52){u->peek_until=now+3500;return FACE_UI_STATUS;}
    u->pat_until=now+1800;++u->pats;return FACE_UI_PAT;
}
