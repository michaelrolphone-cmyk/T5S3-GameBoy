/* Capability-only frontend. The device owns display/storage/input providers;
 * the application owns the emulator, catalogue and presentation policy. */
#include <RiscRuntimeV1.h>
#include <RiscStorageVolumeV1.h>
#include <T5FileOpenApi.h>
#include "model.h"
#include "port.h"
#include "gbemu.h"
#include "audio.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
static const risc_runtime_api_v1* rt;
static const risc_display_output_api_v1* display;
static const risc_input_navigation_api_v1* navigation;
static const risc_storage_volume_api_v1* storage;
static const t5_file_open_api_v1* files;
static risc_runtime_capability_v1 grants[4];
static risc_display_surface_v1 surface;
static risc_display_present_token_v1 pending;
static uint32_t submitted,clock_last;
static uint64_t clock_total;
static bool clock_started,retained,failed,home,playing,redraw,paper;
static unsigned width,height,selected;
static gb_catalog catalog;
static gbemu_t* emulator;
static uint8_t* rom;
static uint8_t* mono;
static char directory[GB_PATH_MAX]="/sd",message[128];
static risc_storage_file_t input_file;
static void hold(void){retained=true;failed=true;if(rt && rt->retain_invocation)(void)rt->retain_invocation();}
static bool alive(void){
 risc_runtime_health_v1 h={0};h.struct_size=sizeof(h);
 if(!rt || retained || !rt->health(&h)){failed=true;return false;}
 return true;
}
int64_t minimal_clock_us(void){
 risc_runtime_health_v1 h={0};h.struct_size=sizeof(h);
 if(!rt || retained || !rt->health(&h)){failed=true;return (int64_t)clock_total*1000;}
 if(!clock_started){clock_last=h.uptime_ms;clock_started=true;}
 clock_total+=(uint32_t)(h.uptime_ms-clock_last);clock_last=h.uptime_ms;
 return (int64_t)clock_total*1000;
}
static uint32_t now(void){return (uint32_t)((uint64_t)minimal_clock_us()/1000);}
static bool poll(risc_input_navigation_frame_v1* out){
 memset(out,0,sizeof(*out));
 if(!alive() || !navigation->poll(navigation->context,out)){failed=true;return false;}
 if(out->pressed&RISC_NAV_HOME)home=true;
 return true;
}
static bool close_input(void){
 if(!input_file)return true;
 risc_storage_file_t f=input_file;input_file=0;
 if(!storage->file_close(storage->context,f,false)){hold();return false;}
 return true;
}
static bool settle(void){
 if(!pending)return true;
 if(!alive())return false;
 risc_display_present_status_v1 status={0};
 if(!display->present_status(display->context,pending,&status) || status.state==RISC_DISPLAY_PRESENT_FAILED){hold();return false;}
 if(status.state==RISC_DISPLAY_PRESENT_COMPLETE || status.state==RISC_DISPLAY_PRESENT_SUPERSEDED){pending=0;return true;}
 if((status.state!=RISC_DISPLAY_PRESENT_QUEUED && status.state!=RISC_DISPLAY_PRESENT_ACTIVE) || (uint32_t)(now()-submitted)>5000){hold();return false;}
 return false;
}
static bool new_surface(void){
 if(pending || !alive())return false;
 memset(&surface,0,sizeof(surface));
 if(!display->acquire(display->context,RISC_DISPLAY_FORMAT_MONO1,&surface)){failed=true;return false;}
 if(!gb_surface(&surface,&width,&height)){failed=true;return false;}
 memset(surface.pixels,0,(size_t)surface.stride_bytes*surface.height);
 return true;
}
static bool submit(bool game){
 risc_display_present_options_v1 options={game && !paper?RISC_DISPLAY_PRESENT_LOW_LATENCY:RISC_DISPLAY_PRESENT_QUALITY,RISC_DISPLAY_QUEUE_FIFO,0};
 risc_display_present_token_v1 token=0;
 if(!display->submit(display->context,surface.frame,NULL,0,&options,&token)){failed=true;return false;}
 surface.frame=0; // Successful submit transfers ownership to the provider.
 if(!token){hold();return false;}
 pending=token;submitted=now();return true;
}
static void draw_picker(void){
 if(!new_surface())return;
 gb_text(&surface,16,12,"GAME BOY - SD LIBRARY",2);
 gb_text(&surface,16,38,directory,1);
 unsigned first=selected/10*10;
 for(unsigned i=0;i<10 && first+i<catalog.count;++i){
  char line[140];gb_entry* entry=&catalog.entries[first+i];
  snprintf(line,sizeof(line),"%c %s%s",first+i==selected?'>':' ',entry->directory?"[DIR] ":"",entry->name);
  gb_text(&surface,16,64+(int)i*28,line,2);
 }
 if(!catalog.count)gb_text(&surface,16,90,"No .gb/.gbc ROMs in this folder",2);
 if(catalog.truncated)gb_text(&surface,16,(int)height-70,"Folder limit reached; use subfolders",1);
 gb_text(&surface,16,(int)height-48,message,1);
 gb_text(&surface,16,(int)height-24,paper?"PAPER  Page Back: mode  Arrows: select  OK: open  Back: parent  Home":"FAST   Page Back: mode  Arrows: select  OK: open  Back: parent  Home",1);
 (void)submit(false);
}
static void draw_game(void){
 if(!new_surface())return;
 gb_text(&surface,10,4,gbemu_get_rom_title(emulator),2);
 gb_text(&surface,(int)width-304,4,"A: OK  B: Back  Hold Back: ROMs  Home",1);
 if(!gb_blit(&surface,mono,GBEMU_FRAMEBUFFER_SIZE)){failed=true;return;}
 (void)submit(true);
}
static bool scan(void){
 memset(&catalog,0,sizeof(catalog));selected=0;message[0]=0;redraw=true;
 if(!alive() || !storage->refresh(storage->context) || !storage->ready(storage->context)){
  strcpy(message,"SD card unavailable");return false;
 }
 risc_storage_dir_t dir=storage->dir_open(storage->context,directory);
 if(!dir){strcpy(message,"Cannot open folder");return false;}
 uint32_t start=now();
 for(unsigned n=0;n<512;++n){
  risc_storage_dirent_v1 entry={0};
  if(!storage->dir_next(storage->context,dir,&entry))break;
  if(!memchr(entry.name,0,sizeof(entry.name))){strcpy(message,"Invalid directory entry");break;}
  (void)gb_catalog_add(&catalog,entry.name,entry.is_directory!=0);
  if(n==511 || (uint32_t)(now()-start)>2000){catalog.truncated=true;break;}
  if(n%8==7){risc_input_navigation_frame_v1 nav;if(!poll(&nav) || home)break;rt->yield_ms(1);}
 }
 if(!alive())return false;
 storage->dir_close(storage->context,dir);
 return !failed;
}
static bool load_rom(const char* path){
 uint64_t bytes=0;
 if(!gb_path(path) || !gb_rom_name(strrchr(path,'/')+1)){strcpy(message,"Invalid ROM path");return false;}
 if(!alive() || !storage->ready(storage->context)){strcpy(message,"SD card unavailable");return false;}
 input_file=storage->file_open_read(storage->context,path,&bytes);
 if(!input_file || bytes<32768 || bytes>GBEMU_MAX_ROM_BYTES){(void)close_input();strcpy(message,"ROM must be 32 KiB to 4 MiB");return false;}
 // The picker has ended the previous play session. Reclaim its ROM before
 // allocating another, so selecting a 4MiB cartridge does not require 8MiB.
 audio_deinit();if(emulator)gbemu_destroy(emulator);emulator=NULL;free(rom);rom=NULL;
 uint8_t* candidate=malloc((size_t)bytes);
 if(!candidate){(void)close_input();strcpy(message,"Not enough memory for this ROM");return false;}
 size_t offset=0;uint32_t start=now();bool cancelled=false;
 while(offset<bytes && !home && !failed){
  size_t count=(size_t)bytes-offset;if(count>4096)count=4096;
  size_t got=storage->file_read(storage->context,input_file,candidate+offset,count);
  if(!got || got>count)break;
  offset+=got;
  risc_input_navigation_frame_v1 nav;if(!poll(&nav))break;
  if(nav.pressed&RISC_NAV_BACK){cancelled=true;break;}
  if((uint32_t)(now()-start)>15000)break;
  rt->yield_ms(1);
 }
 if(retained)return false;
 if(!alive()){hold();return false;}
 if(!close_input())return false;
 if(offset!=bytes || home || failed || cancelled){free(candidate);strcpy(message,"ROM load interrupted");return false;}
 gbemu_t* next=gbemu_create();
 gbemu_status_t status=next?gbemu_init(next,candidate,(size_t)bytes):GBEMU_STATUS_CART_RAM_ALLOC_FAILED;
 if(status!=GBEMU_STATUS_OK){if(next)gbemu_destroy(next);free(candidate);snprintf(message,sizeof(message),"%s",gbemu_status_string(status));return false;}
 if(emulator)gbemu_destroy(emulator);free(rom);emulator=next;rom=candidate;
 audio_set_engine(AUDIO_ENGINE_MUTE);audio_init();audio_set_paused(false);playing=true;redraw=true;
 if(!navigation->reset(navigation->context)){failed=true;return false;}
 return true;
}
static bool cleanup(void){
 if(retained)return false;
 if(!alive()){hold();return false;}
 if(!close_input())return false;
 for(unsigned n=0;pending && n<1002 && !failed;++n){if(settle())break;rt->yield_ms(5);}
 if(pending){hold();return false;}
 if(surface.frame){display->release(display->context,surface.frame);surface.frame=0;}
 for(unsigned i=4;i--;)if(grants[i].api && !rt->release(&grants[i])){hold();return false;}
 audio_deinit();if(emulator)gbemu_destroy(emulator);emulator=NULL;free(rom);rom=NULL;free(mono);mono=NULL;
 return true;
}
__attribute__((visibility("default"))) int app_module_init(void){
 rt=risc_runtime_get_api(1);
 if(!rt || rt->struct_size<RISC_RUNTIME_DEFAULT_REQUEST_V1_SIZE || !rt->request_default || !rt->retain_invocation)return -1;
 const char* caps[]={"display.output","input.navigation","storage.volume","file.open"};
 for(unsigned i=0;i<4;++i){grants[i].struct_size=sizeof(grants[i]);if(!rt->acquire(caps[i],1,0,&grants[i]))goto error;}
 display=grants[0].api;navigation=grants[1].api;storage=grants[2].api;files=grants[3].api;
 if(!display || display->api_version!=1 || display->struct_size<sizeof(*display) || !display->get_info || !display->acquire || !display->release || !display->submit || !display->present_status ||
 !navigation || navigation->api_version!=1 || navigation->struct_size<sizeof(*navigation) || !navigation->poll || !navigation->reset ||
 !storage || storage->api_version!=1 || storage->struct_size<sizeof(*storage) || !storage->refresh || !storage->ready || !storage->dir_open || !storage->dir_next || !storage->dir_close || !storage->file_open_read || !storage->file_read || !storage->file_close ||
 !files || files->api_version!=1 || files->struct_size<sizeof(*files) || !files->source_path_get)goto error;
 {risc_display_info_v1 info={0};if(!display->get_info(display->context,&info) || info.api_version!=1 || info.struct_size<sizeof(info) || !(info.supported_formats&RISC_DISPLAY_FORMAT_BIT(RISC_DISPLAY_FORMAT_MONO1)))goto error;}
 mono=malloc(GBEMU_FRAMEBUFFER_SIZE);if(!mono || !navigation->reset(navigation->context))goto error;
 return 0;
error:
 for(unsigned i=4;i--;)if(grants[i].api && !rt->release(&grants[i])){hold();return -1;}
 free(mono);mono=NULL;return -1;
}
__attribute__((visibility("default"))) void app_main(void){
 char source[GB_PATH_MAX]={0};
 if(files->source_path_get(source,sizeof(source))){
  if(gb_path(source)){strcpy(directory,source);gb_parent(directory);(void)scan();if(!failed && !home)(void)load_rom(source);}
  else{(void)scan();strcpy(message,"Invalid handed-off ROM path");}
  redraw=true;
 }else (void)scan();
 uint32_t next=now(),back=0;bool back_down=false;
 while(!failed && !home){
  risc_input_navigation_frame_v1 nav;if(!poll(&nav) || home)break;
  (void)settle();if(failed)break;
  uint32_t time=now();
  if(playing){
   if(nav.buttons&RISC_NAV_BACK){if(!back_down){back=time;back_down=true;}else if((uint32_t)(time-back)>=900){playing=false;audio_set_paused(true);(void)navigation->reset(navigation->context);(void)scan();}}
   else back_down=false;
   if(playing && (int32_t)(time-next)>=0){
    if(!gbemu_run_frame(emulator,mono,GBEMU_FRAMEBUFFER_SIZE,gb_buttons(nav.buttons),false,NULL)){snprintf(message,sizeof(message),"%s",gbemu_get_last_error_string(emulator));playing=false;redraw=true;}
    else{audio_service_frame();redraw=true;}
    next=time+17; // Bounded one-frame work; no catch-up burst after slow I/O.
   }
  }else{
   if(nav.pressed&RISC_NAV_BACK){if(!strcmp(directory,"/sd"))home=true;else{gb_parent(directory);(void)scan();}}
   int delta=(nav.pressed&(RISC_NAV_UP|RISC_NAV_LEFT))?-1:(nav.pressed&(RISC_NAV_DOWN|RISC_NAV_RIGHT))?1:0;
   if(nav.pressed&RISC_NAV_PAGE_BACK){paper=!paper;redraw=true;}if(nav.pressed&RISC_NAV_PAGE_FORWARD)delta=10;
   if(delta && catalog.count){int value=(int)selected+delta;if(value<0)value=0;if(value>=(int)catalog.count)value=(int)catalog.count-1;selected=(unsigned)value;redraw=true;}
   if((nav.pressed&RISC_NAV_CONFIRM) && catalog.count){
    char path[GB_PATH_MAX];gb_entry* entry=&catalog.entries[selected];
    if(gb_join(directory,entry->name,path,sizeof(path))){if(entry->directory){strcpy(directory,path);(void)scan();}else{(void)load_rom(path);next=now();redraw=true;}}
   }
  }
  if(redraw && !pending && !failed && !home){if(playing)draw_game();else draw_picker();redraw=false;}
  if(!failed && !home)rt->yield_ms(playing?1:10);
 }
 bool clean=cleanup();
 if(home && clean && !rt->request_default())(void)rt->diagnostic("GameBoy: Home request refused");
}
__attribute__((visibility("default"))) void app_module_fini(void){
 if(retained)return;
 if(mono || emulator || grants[0].api)(void)cleanup();
}
