#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef enum {FACE_UI_NONE,FACE_UI_PAT,FACE_UI_STATUS,FACE_UI_LISTEN,FACE_UI_REST,
    FACE_UI_QUIETER,FACE_UI_LOUDER,FACE_UI_CLOSE,FACE_UI_PAIR} face_ui_action;
typedef struct {bool menu,down,long_fired,moved;int x,y;uint32_t began,menu_at,peek_until,pat_until;
    unsigned pats,long_presses;} face_ui;
face_ui_action face_ui_update(face_ui *ui,bool pressed,int x,int y,uint32_t now);
bool face_ui_peeking(const face_ui *ui,uint32_t now);
bool face_ui_patted(const face_ui *ui,uint32_t now);
