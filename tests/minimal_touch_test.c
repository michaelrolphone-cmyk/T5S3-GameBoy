#include "touch.h"
#include "gbemu.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static risc_touch_snapshot_v1 snapshot(bool portrait,uint64_t time){
 risc_touch_snapshot_v1 s={0};s.width=portrait?480:800;s.height=portrait?800:480;s.timestamp_ms=time;return s;
}
static void put(risc_touch_snapshot_v1* s,unsigned n,uint8_t id,int x,int y){
 s->contacts[n]=(risc_touch_contact_v1){id,0,(uint16_t)(s->height>s->width?479-y:x),(uint16_t)(s->height>s->width?x:y)};
 if(s->contact_count<n+1)s->contact_count=(uint8_t)(n+1);
}
static gb_touch_output sample(gb_touch* t,risc_touch_snapshot_v1* s,uint32_t now){return gb_touch_sample(t,s,now,800,480,true);}
static void controls(bool portrait){
 gb_touch t={0};risc_touch_snapshot_v1 s=snapshot(portrait,1);gb_layout l;gb_layout_make(800,480,&l);
 put(&s,0,16,128,240);put(&s,1,2,756,180);
 assert(sample(&t,&s,1).buttons==(GBEMU_INPUT_RIGHT|GBEMU_INPUT_A));
 risc_touch_contact_v1 swap=s.contacts[0];s.contacts[0]=s.contacts[1];s.contacts[1]=swap;
 s.timestamp_ms=2;assert(sample(&t,&s,2).buttons==(GBEMU_INPUT_RIGHT|GBEMU_INPUT_A));
 put(&s,2,11,684,260);put(&s,3,7,64,390);put(&s,4,15,728,390);
 assert(sample(&t,&s,3).buttons==(GBEMU_INPUT_RIGHT|GBEMU_INPUT_A|GBEMU_INPUT_B|GBEMU_INPUT_SELECT|GBEMU_INPUT_START));
 // Missing IDs release independently. A replacement does not inherit the A role.
 s.contact_count=2;s.contacts[0]=s.contacts[1];put(&s,1,6,684,260);s.timestamp_ms=4;
 assert(sample(&t,&s,4).buttons==(GBEMU_INPUT_RIGHT|GBEMU_INPUT_B));
 assert(!t.fingers[2].active && !t.fingers[7].active && !t.fingers[11].active && !t.fingers[15].active);
 // A retained D-pad contact slides diagonally while the independent B stays held.
 put(&s,0,16,128,180);assert(sample(&t,&s,5).buttons==(GBEMU_INPUT_RIGHT|GBEMU_INPUT_UP|GBEMU_INPUT_B));
 put(&s,0,16,20,300);assert(sample(&t,&s,6).buttons==(GBEMU_INPUT_LEFT|GBEMU_INPUT_DOWN|GBEMU_INPUT_B));
 put(&s,0,16,80,240);assert(sample(&t,&s,7).buttons==GBEMU_INPUT_B);
 put(&s,0,16,300,240);assert(sample(&t,&s,8).buttons==GBEMU_INPUT_B);
 // UP retires the ID; a subsequent DOWN on A can capture a different role.
 risc_touch_event_v1 e={0};e.kind=RISC_TOUCH_EVENT_UP;e.id=16;gb_touch_event(&t,&e);assert(!t.fingers[16].active);
 put(&s,0,16,756,180);assert(sample(&t,&s,9).buttons==(GBEMU_INPUT_A|GBEMU_INPUT_B));
 s.contact_count=0;s.timestamp_ms=10;assert(!sample(&t,&s,10).buttons);
 // Contact born outside controls cannot turn into a key by dragging into one.
 put(&s,0,1,300,240);assert(!sample(&t,&s,11).buttons);put(&s,0,1,756,180);assert(!sample(&t,&s,12).buttons);
 s.contact_count=0;assert(!sample(&t,&s,13).buttons);
 // Every public ID, including 16, owns its own release/capture state.
 for(unsigned id=1;id<=16;++id){put(&s,0,(uint8_t)id,756,180);assert(sample(&t,&s,14+id*2).buttons==GBEMU_INPUT_A);s.contact_count=0;assert(!sample(&t,&s,15+id*2).buttons);}
 // Static controller snapshots expire; new hardware report timestamps keep a hold alive.
 put(&s,0,16,756,180);s.timestamp_ms=100;assert(sample(&t,&s,100).buttons==GBEMU_INPUT_A);
 assert(sample(&t,&s,599).buttons==GBEMU_INPUT_A);assert(!sample(&t,&s,600).buttons && t.blocked);
 s.timestamp_ms=601;assert(!sample(&t,&s,601).buttons);s.contact_count=0;s.timestamp_ms=602;assert(!sample(&t,&s,602).buttons && !t.blocked);
 put(&s,0,16,756,180);
 for(unsigned n=0;n<20;++n){s.timestamp_ms=603+n*100;assert(sample(&t,&s,603+n*100).buttons==GBEMU_INPUT_A);}
 // GAP and explicit cancel must not reassert a finger still on the screen.
 gb_touch_cancel(&t);s.timestamp_ms=2600;assert(!sample(&t,&s,2600).buttons && t.blocked);
 s.contact_count=0;s.timestamp_ms=2601;assert(!sample(&t,&s,2601).buttons && !t.blocked);
 put(&s,0,16,756,180);assert(sample(&t,&s,2602).buttons==GBEMU_INPUT_A);
 e.kind=255;gb_touch_event(&t,&e);assert(!sample(&t,&s,2603).buttons && t.blocked);
 // A malformed report cancels rather than publishing a valid prefix.
 const unsigned bad[]={0,17};
 for(unsigned n=0;n<2;++n){memset(&t,0,sizeof(t));s=snapshot(portrait,1);put(&s,0,2,756,180);put(&s,1,(uint8_t)bad[n],128,240);assert(!sample(&t,&s,1).buttons && t.blocked);}
 memset(&t,0,sizeof(t));s=snapshot(portrait,1);put(&s,0,2,756,180);put(&s,1,2,128,240);assert(!sample(&t,&s,1).buttons && t.blocked);
 memset(&t,0,sizeof(t));s=snapshot(portrait,1);put(&s,0,2,756,180);s.contacts[0].x=s.width;assert(!sample(&t,&s,1).buttons && t.blocked);
 memset(&t,0,sizeof(t));s=snapshot(portrait,1);s.contact_count=6;assert(!sample(&t,&s,1).buttons && t.blocked);
 memset(&t,0,sizeof(t));s=snapshot(portrait,100);put(&s,0,2,756,180);assert(sample(&t,&s,100).buttons);s.timestamp_ms=99;assert(!sample(&t,&s,101).buttons && t.blocked);
 // Unsigned local time wrapping does not prolong a stale hold.
 memset(&t,0,sizeof(t));s=snapshot(portrait,1);put(&s,0,2,756,180);assert(sample(&t,&s,UINT32_MAX-200).buttons);assert(!sample(&t,&s,299).buttons && t.blocked);
}
static void actions(void){
 gb_touch t={0};risc_touch_snapshot_v1 s=snapshot(true,1);put(&s,0,1,64,20);
 assert(sample(&t,&s,1).actions==GB_TOUCH_HOME);assert(!sample(&t,&s,2).actions);
 s.contact_count=0;sample(&t,&s,3);put(&s,0,1,64,80);assert(sample(&t,&s,4).actions==GB_TOUCH_ROMS);
 s.contact_count=0;sample(&t,&s,5);put(&s,0,1,64,440);assert(sample(&t,&s,6).actions==GB_TOUCH_MODE);
 memset(&t,0,sizeof(t));s=snapshot(true,1);s.buttons=RISC_TOUCH_BUTTON_PRIMARY;assert(sample(&t,&s,1).actions==GB_TOUCH_HOME);assert(!sample(&t,&s,2).actions);
 memset(&t,0,sizeof(t));s=snapshot(true,1);put(&s,0,1,240,64+28*4+12);
 gb_touch_output out=gb_touch_sample(&t,&s,1,800,480,false);assert(out.actions==GB_TOUCH_PICK && out.row==4 && !out.buttons);
 assert(!gb_touch_sample(&t,&s,2,800,480,false).actions);
}
static void render(void){
 for(unsigned portrait=0;portrait<2;++portrait){
  unsigned w=portrait?480:800,h=portrait?800:480,stride=(w+7)/8,bytes=stride*h;
  uint8_t* memory=malloc(bytes+32);assert(memory);memset(memory,0xa5,bytes+32);memset(memory+16,0,bytes);
  risc_display_surface_v1 s={1,memory+16,w,h,stride,bytes,1};
  gb_draw_controls(&s,true,false,255);
  // Inverse touch coordinates land on the same physical D-pad border pixels.
  unsigned x=8,y=216,px=portrait?479-y:x,py=portrait?x:y;
  assert(((uint8_t*)s.pixels)[py*stride+px/8]&(0x80u>>(px&7)));
  for(unsigned i=0;i<16;++i)assert(memory[i]==0xa5 && memory[bytes+16+i]==0xa5);
  gb_draw_controls(&s,false,true,0);free(memory);
 }
 gb_layout l;gb_layout_make(480,432,&l);assert(l.scale==1 && l.game_x>=152);
 gb_layout_make(800,480,&l);assert(l.scale==3 && l.game_x==160 && l.game_y==34);
}
int main(void){controls(false);controls(true);actions();render();puts("Touch: simultaneous five-contact controls, ID ownership, cancellation, timeout, actions and orientation PASS");}
