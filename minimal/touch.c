#include "touch.h"
#include "gbemu.h"
#include <string.h>
enum { ROLE_NONE,ROLE_DPAD,ROLE_A,ROLE_B,ROLE_SELECT,ROLE_START,ROLE_ACTION };
static bool inside(gb_rect r,int x,int y){return x>=r.x && y>=r.y && x<r.x+r.w && y<r.y+r.h;}
void gb_layout_make(unsigned width,unsigned height,gb_layout* l){
 int w=(int)width,h=(int)height;memset(l,0,sizeof(*l));
 unsigned sx=width>288?(width-288)/160:1,sy=height>32?(height-32)/144:1;
 l->scale=sx<sy?sx:sy;if(l->scale>3)l->scale=3;if(!l->scale)l->scale=1;
 l->game_x=(w-(int)(160*l->scale))/2;l->game_y=(h-(int)(144*l->scale))/2+10;
 l->dpad=(gb_rect){8,h/2-72,144,144};
 l->a=(gb_rect){w-80,h/2-86,72,72};l->b=(gb_rect){w-152,h/2-8,72,72};
 l->select=(gb_rect){8,h-110,136,44};l->start=(gb_rect){w-144,h-110,136,44};
 l->home=(gb_rect){8,4,136,44};l->roms=(gb_rect){8,64,136,44};l->mode=(gb_rect){8,h-54,136,44};
 l->back=(gb_rect){96,h-48,80,40};l->prev=(gb_rect){w-176,h-48,80,40};l->next=(gb_rect){w-88,h-48,80,40};
}
static void box(risc_display_surface_v1* s,gb_rect r,const char* label,bool held){
 for(int k=0;k<(held?3:1);++k){
  for(int x=r.x+k;x<r.x+r.w-k;++x){gb_pixel(s,x,r.y+k,true);gb_pixel(s,x,r.y+r.h-1-k,true);}
  for(int y=r.y+k;y<r.y+r.h-k;++y){gb_pixel(s,r.x+k,y,true);gb_pixel(s,r.x+r.w-1-k,y,true);}
 }
 int len=(int)strlen(label);unsigned scale=r.w>=64 && len<=6?2:1;
 gb_text(s,r.x+(r.w-len*(int)(8*scale))/2,r.y+(r.h-(int)(8*scale))/2,label,scale);
}
void gb_draw_controls(risc_display_surface_v1* s,bool game,bool paper,uint8_t held){
 unsigned w,h;if(!gb_surface(s,&w,&h))return;gb_layout l;gb_layout_make(w,h,&l);
 if(game){
  box(s,l.home,"HOME",false);box(s,l.roms,"ROMS",false);box(s,l.mode,paper?"PAPER":"FAST",false);
  box(s,l.a,"A",held&GBEMU_INPUT_A);box(s,l.b,"B",held&GBEMU_INPUT_B);
  box(s,l.select,"SELECT",held&GBEMU_INPUT_SELECT);box(s,l.start,"START",held&GBEMU_INPUT_START);
  const gb_rect r=l.dpad;
  box(s,(gb_rect){r.x+48,r.y,48,48},"UP",held&GBEMU_INPUT_UP);
  box(s,(gb_rect){r.x,r.y+48,48,48},"<",held&GBEMU_INPUT_LEFT);
  box(s,(gb_rect){r.x+96,r.y+48,48,48},">",held&GBEMU_INPUT_RIGHT);
  box(s,(gb_rect){r.x+48,r.y+96,48,48},"DOWN",held&GBEMU_INPUT_DOWN);
 }else{
  box(s,(gb_rect){8,(int)h-48,80,40},"HOME",false);box(s,l.back,"BACK",false);
  box(s,(gb_rect){184,(int)h-48,104,40},paper?"PAPER":"FAST",false);
  box(s,l.prev,"PREV",false);box(s,l.next,"NEXT",false);
 }
}
void gb_touch_cancel(gb_touch* t){
 memset(t->fingers,0,sizeof(t->fingers));t->blocked=true;t->primary=false;
}
void gb_touch_event(gb_touch* t,const risc_touch_event_v1* e){
 if(!e){gb_touch_cancel(t);return;}
 switch(e->kind){
 case RISC_TOUCH_EVENT_UP:
  if(e->id && e->id<=GB_TOUCH_IDS)memset(&t->fingers[e->id],0,sizeof(t->fingers[e->id]));
  else gb_touch_cancel(t);
  break;
 case RISC_TOUCH_EVENT_DOWN:case RISC_TOUCH_EVENT_MOVE:
  if(!e->id || e->id>GB_TOUCH_IDS)gb_touch_cancel(t);
  break;
 case RISC_TOUCH_EVENT_BUTTON_DOWN:case RISC_TOUCH_EVENT_BUTTON_UP:
  if(e->id)gb_touch_cancel(t);
  break;
 default:gb_touch_cancel(t);break; /* Future/unknown events cannot latch keys. */
 }
}
static uint8_t role(const gb_layout* l,int x,int y){
 if(inside(l->dpad,x,y))return ROLE_DPAD;if(inside(l->a,x,y))return ROLE_A;
 if(inside(l->b,x,y))return ROLE_B;if(inside(l->select,x,y))return ROLE_SELECT;
 if(inside(l->start,x,y))return ROLE_START;return ROLE_NONE;
}
static uint8_t buttons(const gb_layout* l,uint8_t r,int x,int y){
 if(r==ROLE_A)return inside(l->a,x,y)?GBEMU_INPUT_A:0;
 if(r==ROLE_B)return inside(l->b,x,y)?GBEMU_INPUT_B:0;
 if(r==ROLE_SELECT)return inside(l->select,x,y)?GBEMU_INPUT_SELECT:0;
 if(r==ROLE_START)return inside(l->start,x,y)?GBEMU_INPUT_START:0;
 if(r!=ROLE_DPAD || !inside(l->dpad,x,y))return 0;
 x-=l->dpad.x;y-=l->dpad.y;
 return (x<48?GBEMU_INPUT_LEFT:x>=96?GBEMU_INPUT_RIGHT:0) |
        (y<48?GBEMU_INPUT_UP:y>=96?GBEMU_INPUT_DOWN:0);
}
gb_touch_output gb_touch_sample(gb_touch* t,const risc_touch_snapshot_v1* s,uint32_t now,
                              unsigned width,unsigned height,bool game){
 gb_touch_output out={0};uint32_t present=0;
 if(!s || s->contact_count>RISC_TOUCH_MAX_CONTACTS || s->buttons&~RISC_TOUCH_BUTTON_PRIMARY ||
    (s->width>s->height?s->width:s->height)!=width || (s->width>s->height?s->height:s->width)!=height){gb_touch_cancel(t);return out;}
 for(unsigned i=0;i<s->contact_count;++i){
  const risc_touch_contact_v1* c=&s->contacts[i];
  if(!c->id || c->id>GB_TOUCH_IDS || c->x>=s->width || c->y>=s->height || (present&(1u<<c->id))){gb_touch_cancel(t);return out;}
  present|=1u<<c->id;
 }
 // Two picker taps must not select a row according to controller array order.
 if(!game && s->contact_count>1){gb_touch_cancel(t);return out;}
 bool fresh=!t->seen || s->timestamp_ms!=t->timestamp || s->sequence!=t->sequence;
 if(t->seen && (s->timestamp_ms<t->timestamp || s->sequence<t->sequence)){gb_touch_cancel(t);}
 if(fresh){t->seen=true;t->timestamp=s->timestamp_ms;t->sequence=s->sequence;t->last_report=now;}
 if((uint32_t)(now-t->last_report)>=GB_TOUCH_TIMEOUT_MS){gb_touch_cancel(t);return out;}
 if(t->blocked){if(!s->contact_count && !s->buttons)t->blocked=false;return out;}
 if((s->buttons&RISC_TOUCH_BUTTON_PRIMARY) && !t->primary)out.actions|=GB_TOUCH_HOME;
 t->primary=(s->buttons&RISC_TOUCH_BUTTON_PRIMARY)!=0;
 for(unsigned id=1;id<=GB_TOUCH_IDS;++id)if(!(present&(1u<<id)))memset(&t->fingers[id],0,sizeof(t->fingers[id]));
 gb_layout l;gb_layout_make(width,height,&l);
 for(unsigned i=0;i<s->contact_count;++i){
  const risc_touch_contact_v1* c=&s->contacts[i];int x=c->x,y=c->y;
  if(s->height>s->width){x=c->y;y=(int)s->width-1-c->x;}
  gb_finger* f=&t->fingers[c->id];
  if(!f->active){
   f->active=true;f->role=game?role(&l,x,y):ROLE_NONE;f->action=0;
   if(game){
    if(inside(l.home,x,y))f->action=GB_TOUCH_HOME;
    else if(inside(l.roms,x,y))f->action=GB_TOUCH_ROMS;
    else if(inside(l.mode,x,y))f->action=GB_TOUCH_MODE;
   }else{
    if(inside((gb_rect){8,(int)height-48,80,40},x,y))f->action=GB_TOUCH_HOME;
    else if(inside(l.back,x,y))f->action=GB_TOUCH_BACK;
    else if(inside((gb_rect){184,(int)height-48,104,40},x,y))f->action=GB_TOUCH_MODE;
    else if(inside(l.prev,x,y))f->action=GB_TOUCH_PREV;
    else if(inside(l.next,x,y))f->action=GB_TOUCH_NEXT;
    else if(x>=8 && x<(int)width-8 && y>=64 && y<344){f->action=GB_TOUCH_PICK;out.row=(unsigned)(y-64)/28;}
   }
   if(f->action){f->role=ROLE_ACTION;out.actions|=f->action;}
  }
  out.buttons|=buttons(&l,f->role,x,y);
 }
 return out;
}
