#pragma once
#include "model.h"
#include "RiscTouchV1.h"
/* Public GT911 IDs are 1..16; at most five can be present in one report. */
#define GB_TOUCH_IDS 16u
#define GB_TOUCH_TIMEOUT_MS 500u
enum { GB_TOUCH_HOME=1u, GB_TOUCH_ROMS=2u, GB_TOUCH_MODE=4u,
       GB_TOUCH_PICK=8u, GB_TOUCH_BACK=16u, GB_TOUCH_PREV=32u, GB_TOUCH_NEXT=64u };
typedef struct {int x,y,w,h;} gb_rect;
typedef struct {
 gb_rect dpad,a,b,select,start,home,roms,mode,back,prev,next;
 unsigned scale;int game_x,game_y;
} gb_layout;
typedef struct {bool active;uint8_t role;uint32_t action;} gb_finger;
typedef struct {
 gb_finger fingers[GB_TOUCH_IDS+1];
 uint64_t sequence,timestamp;
 uint32_t last_report;
 bool blocked,seen,primary;
} gb_touch;
typedef struct {uint8_t buttons;uint32_t actions;unsigned row;} gb_touch_output;
void gb_layout_make(unsigned width,unsigned height,gb_layout*);
void gb_draw_controls(risc_display_surface_v1*,bool game,bool paper,uint8_t buttons);
void gb_touch_cancel(gb_touch*);
void gb_touch_event(gb_touch*,const risc_touch_event_v1*);
gb_touch_output gb_touch_sample(gb_touch*,const risc_touch_snapshot_v1*,uint32_t now,
                              unsigned width,unsigned height,bool game);
