/* Capability-only frontend. The device owns display/storage/input providers;
 * the application owns the emulator, catalogue and presentation policy. */
#include <RiscRuntimeV1.h>
#include <RiscStorageVolumeV1.h>
#include <T5FileOpenApi.h>
#include "model.h"
#include "touch.h"
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
static const risc_touch_api_v1* touch;
static risc_runtime_capability_v1 grants[5];
static uint64_t touch_subscription;
static gb_touch touch_state;
static gb_touch_output touch_input;
static uint32_t touch_polled;
static bool touch_fault_reported;
static risc_display_surface_v1 surface;
static risc_display_present_token_v1 pending;
static uint32_t submitted,clock_last;
static bool first_present=true,first_game=true;
static uint8_t logged_present_state=255,first_present_logs;
static char failure[128];
static const char* cap_names[]={"display.output","input.navigation","storage.volume","file.open","input.touch.raw"};
static uint64_t clock_total;
static bool clock_started,retained,failed,home,playing,redraw,paper;
static unsigned width,height,selected;
static gb_catalog catalog;
static gbemu_t* emulator;
static uint8_t* rom;
static uint8_t* mono;
static char directory[GB_PATH_MAX]="/sd",message[128];
static risc_storage_file_t input_file;
static void trace(const char* stage,const char* result,const char* detail){
 if(!rt || !rt->diagnostic || retained)return;
 risc_runtime_health_v1 h={0};h.struct_size=sizeof(h);uint32_t time=clock_last;
 if(rt->health && rt->health(&h))time=h.uptime_ms;
 char line[240];snprintf(line,sizeof(line),"GAMEBOY t_ms=%lu stage=%s result=%s %.112s",(unsigned long)time,stage,result,detail?detail:"");
 for(unsigned i=0;line[i];++i)if((unsigned char)line[i]<32)line[i]=' ';
 (void)rt->diagnostic(line);
}
static void fail(const char* operation,const char* detail){
 failed=true;if(!failure[0])snprintf(failure,sizeof(failure),"%s %.88s",operation,detail?detail:"");
 trace(operation,"failed",detail);
}
static void hold(const char* operation){
 fail(operation,"ownership not proven released");char detail[112];snprintf(detail,sizeof(detail),"operation=%s first=%.56s",operation,failure);trace("retain-invocation","required",detail);
 retained=true;if(rt && rt->retain_invocation)(void)rt->retain_invocation();
}
static void storage_error(const char* operation){
 char detail[112]={0};if(storage->last_error)(void)storage->last_error(storage->context,detail,sizeof(detail));
 detail[sizeof(detail)-1]=0;trace(operation,"failed",detail);
}
static bool alive(void){
 risc_runtime_health_v1 h={0};h.struct_size=sizeof(h);
 if(!rt || retained || !rt->health(&h)){fail("runtime-health","unavailable");return false;}
 return true;
}
int64_t minimal_clock_us(void){
 risc_runtime_health_v1 h={0};h.struct_size=sizeof(h);
 if(!rt || retained || !rt->health(&h)){fail("runtime-clock","health unavailable");return (int64_t)clock_total*1000;}
 if(!clock_started){clock_last=h.uptime_ms;clock_started=true;}
 clock_total+=(uint32_t)(h.uptime_ms-clock_last);clock_last=h.uptime_ms;
 return (int64_t)clock_total*1000;
}
static uint32_t now(void){return (uint32_t)((uint64_t)minimal_clock_us()/1000);}
static void cancel_touch(void){gb_touch_cancel(&touch_state);memset(&touch_input,0,sizeof(touch_input));}
static void touch_fault(const char* reason){
 if(!touch_fault_reported)trace("touch-input","cancelled",reason);
 touch_fault_reported=true;cancel_touch();
}
static void poll_touch(void){
 touch_input.actions=0;
 uint32_t time=now();if((uint32_t)(time-touch_polled)<8)return;touch_polled=time;
 bool blocked_before=touch_state.blocked;
 bool healthy=touch->poll(touch->context,16);bool drained=false;
 // A failed poll can still have produced events. Always drain the subscription.
 for(unsigned n=0;n<RISC_TOUCH_QUEUE_LENGTH;++n){
  risc_touch_event_v1 event={0};int32_t result=touch->next(touch->context,touch_subscription,&event);
  if(!result){drained=true;break;}if(result!=1){healthy=false;break;}
  gb_touch_event(&touch_state,&event);
 }
 if(!healthy || !drained)touch_fault("poll failure or event gap");
 risc_touch_snapshot_v1 snapshot={0};
 if(!touch->snapshot(touch->context,&snapshot)){touch_fault("snapshot unavailable");return;}
 touch_input=gb_touch_sample(&touch_state,&snapshot,time,width,height,playing);
 if(!blocked_before && touch_state.blocked && !touch_fault_reported){trace("touch-input","cancelled","invalid event or stale snapshot");touch_fault_reported=true;}
 if(touch_fault_reported && healthy && !touch_state.blocked){trace("touch-input","recovered","neutral snapshot");touch_fault_reported=false;}
 if(touch_input.actions&GB_TOUCH_HOME)home=true;
}
static bool poll(risc_input_navigation_frame_v1* out){
 memset(out,0,sizeof(*out));
 if(!alive() || !navigation->poll(navigation->context,out)){fail("input-poll","navigation unavailable");return false;}
 if(out->pressed&RISC_NAV_HOME)home=true;
 poll_touch();
 if(!playing && (touch_input.actions&GB_TOUCH_BACK))out->pressed|=RISC_NAV_BACK;
 return true;
}
static bool close_input(void){
 if(!input_file)return true;
 risc_storage_file_t f=input_file;trace("rom-close","begin",NULL);
 if(!storage->file_close(storage->context,f,false)){storage_error("rom-close");hold("rom-close");return false;}
 input_file=0;trace("rom-close","ok",NULL);return true;
}
static bool settle(void){
 if(!pending)return true;
 if(!alive())return false;
 risc_display_present_status_v1 status={0};
 if(!display->present_status(display->context,pending,&status)){hold("display-present-status");return false;}
 if(first_present && first_present_logs<8 && status.state!=logged_present_state){char detail[64];snprintf(detail,sizeof(detail),"state=%u elapsed_ms=%lu",status.state,(unsigned long)(now()-submitted));trace("first-present-wait","progress",detail);logged_present_state=status.state;++first_present_logs;}
 if(status.state==RISC_DISPLAY_PRESENT_FAILED){hold("display-present-failed");return false;}
 if(status.state==RISC_DISPLAY_PRESENT_COMPLETE || status.state==RISC_DISPLAY_PRESENT_SUPERSEDED){pending=0;if(first_present){trace("first-present-wait","complete",NULL);first_present=false;}return true;}
 if(status.state!=RISC_DISPLAY_PRESENT_QUEUED && status.state!=RISC_DISPLAY_PRESENT_ACTIVE){hold("display-present-state");return false;}
 if((uint32_t)(now()-submitted)>5000){hold("display-present-timeout");return false;}
 return false;
}
static bool new_surface(void){
 if(pending || !alive())return false;
 memset(&surface,0,sizeof(surface));
 if(!display->acquire(display->context,RISC_DISPLAY_FORMAT_MONO1,&surface)){fail("display-frame-acquire","refused");return false;}
 if(!gb_surface(&surface,&width,&height)){fail("display-surface","invalid geometry");return false;}
 memset(surface.pixels,0,(size_t)surface.stride_bytes*surface.height);
 return true;
}
static bool submit(bool game){
 risc_display_present_options_v1 options={!paper?RISC_DISPLAY_PRESENT_LOW_LATENCY:RISC_DISPLAY_PRESENT_QUALITY,RISC_DISPLAY_QUEUE_FIFO,0};
 risc_display_present_token_v1 token=0;
 if(first_present || (game && first_game))trace(game?"game-first-frame":"menu-first-frame","submit",paper?"intent=quality":"intent=low-latency");
 if(!display->submit(display->context,surface.frame,NULL,0,&options,&token)){fail("display-submit","refused");return false;}
 surface.frame=0; // Successful submit transfers ownership to the provider.
 if(!token){hold("display-submit-token");return false;}
 pending=token;submitted=now();if(game)first_game=false;return true;
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
 if(catalog.truncated)gb_text(&surface,16,(int)height-84,"Folder limit reached; use subfolders",1);
 gb_text(&surface,16,(int)height-68,message,1);
 gb_draw_controls(&surface,false,paper,0);
 (void)submit(false);
}
static void draw_game(void){
 if(!new_surface())return;
 gb_layout layout;gb_layout_make(width,height,&layout);
 gb_text(&surface,layout.game_x,4,gbemu_get_rom_title(emulator),1);
 if(!gb_blit(&surface,mono,GBEMU_FRAMEBUFFER_SIZE)){fail("game-blit","invalid surface");return;}
 gb_draw_controls(&surface,true,paper,touch_input.buttons);
 (void)submit(true);
}
static bool scan(void){
 memset(&catalog,0,sizeof(catalog));selected=0;message[0]=0;redraw=true;trace("menu-scan","begin",directory);
 if(!alive())return false;
 trace("storage-refresh","begin",NULL);
 if(!storage->refresh(storage->context)){strcpy(message,"SD card unavailable");storage_error("storage-refresh");return false;}
 trace("storage-refresh","ok",NULL);
 if(!storage->ready(storage->context)){strcpy(message,"SD card unavailable");storage_error("storage-ready");return false;}
 trace("storage-ready","ok",NULL);
 trace("storage-dir-open","begin",gb_volume_path(directory));
 risc_storage_dir_t dir=storage->dir_open(storage->context,gb_volume_path(directory));
 if(!dir){strcpy(message,"Cannot open folder");storage_error("storage-dir-open");return false;}
 trace("storage-dir-open","ok",NULL);
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
 trace("storage-dir-close","begin",NULL);storage->dir_close(storage->context,dir);trace("storage-dir-close","returned",NULL);
 char detail[64];snprintf(detail,sizeof(detail),"entries=%u truncated=%u",(unsigned)catalog.count,catalog.truncated);trace("menu-scan","complete",detail);return !failed;
}
static bool load_rom(const char* path){
 uint64_t bytes=0;trace("rom-load","begin",path);
 if(!gb_path(path) || !gb_rom_name(strrchr(path,'/')+1)){strcpy(message,"Invalid ROM path");trace("rom-path","failed",path);return false;}
 if(!alive() || !storage->ready(storage->context)){strcpy(message,"SD card unavailable");storage_error("storage-ready");return false;}
 trace("rom-open","begin",gb_volume_path(path));input_file=storage->file_open_read(storage->context,gb_volume_path(path),&bytes);
 if(!input_file)storage_error("rom-open");else trace("rom-open","ok",NULL);
 if(!input_file || bytes<32768 || bytes>GBEMU_MAX_ROM_BYTES){(void)close_input();strcpy(message,"ROM must be 32 KiB to 4 MiB");trace("rom-size","failed",message);return false;}
 // The picker has ended the previous play session. Reclaim its ROM before
 // allocating another, so selecting a 4MiB cartridge does not require 8MiB.
 audio_deinit();if(emulator)gbemu_destroy(emulator);emulator=NULL;free(rom);rom=NULL;
 uint8_t* candidate=malloc((size_t)bytes);
 if(!candidate){(void)close_input();strcpy(message,"Not enough memory for this ROM");trace("rom-allocate","failed",message);return false;}
 size_t offset=0;uint32_t start=now();bool cancelled=false;
 while(offset<bytes && !home && !failed){
  size_t count=(size_t)bytes-offset;if(count>4096)count=4096;
  size_t got=storage->file_read(storage->context,input_file,candidate+offset,count);
  if(!got || got>count){storage_error("rom-read");break;}
  offset+=got;
  risc_input_navigation_frame_v1 nav;if(!poll(&nav))break;
  if(nav.pressed&RISC_NAV_BACK){cancelled=true;break;}
  if((uint32_t)(now()-start)>15000){trace("rom-read","failed","deadline exceeded");break;}
  rt->yield_ms(1);
 }
 if(retained)return false;
 trace("rom-read","closing",NULL);
 if(!alive()){hold("rom-read-health");return false;}
 if(!close_input())return false;
 if(offset!=bytes || home || failed || cancelled){trace("rom-load","interrupted",cancelled?"cancelled":"incomplete read or exit");free(candidate);strcpy(message,"ROM load interrupted");return false;}
 trace("rom-read","complete",NULL);trace("rom-core-init","begin",NULL);
 gbemu_t* next=gbemu_create();
 gbemu_status_t status=next?gbemu_init(next,candidate,(size_t)bytes):GBEMU_STATUS_CART_RAM_ALLOC_FAILED;
 if(status!=GBEMU_STATUS_OK){trace("rom-core-init","failed",gbemu_status_string(status));if(next)gbemu_destroy(next);free(candidate);snprintf(message,sizeof(message),"%s",gbemu_status_string(status));return false;}
 if(emulator)gbemu_destroy(emulator);free(rom);emulator=next;rom=candidate;
 trace("rom-core-init","ok",NULL);audio_set_engine(AUDIO_ENGINE_MUTE);audio_init();audio_set_paused(false);playing=true;redraw=true;
 cancel_touch(); // Opening fingers cannot become gameplay keys.
 if(!navigation->reset(navigation->context)){fail("input-reset","refused");return false;}
 trace("rom-load","complete",NULL);
 return true;
}
static bool cleanup(void){
 if(retained)return false;
 trace("cleanup","begin",failure);
 if(!alive()){hold("cleanup-health");return false;}
 if(!close_input())return false;
 for(unsigned n=0;pending && n<1002;++n){if(settle())break;if(retained)return false;rt->yield_ms(5);}
 if(pending){hold("cleanup-present-pending");return false;}
 if(surface.frame){trace("display-frame-release","begin",NULL);display->release(display->context,surface.frame);surface.frame=0;trace("display-frame-release","returned",NULL);}
 cancel_touch();
 if(touch_subscription){trace("touch-unsubscribe","begin",NULL);if(!touch->unsubscribe(touch->context,touch_subscription)){hold("touch-unsubscribe");return false;}touch_subscription=0;trace("touch-unsubscribe","ok",NULL);}
 for(unsigned i=5;i--;)if(grants[i].api){trace("capability-release","begin",cap_names[i]);if(!rt->release(&grants[i])){trace("capability-release","failed",cap_names[i]);hold("capability-release");return false;}trace("capability-release","ok",cap_names[i]);}
 audio_deinit();if(emulator)gbemu_destroy(emulator);emulator=NULL;free(rom);rom=NULL;free(mono);mono=NULL;
 trace("cleanup","complete",NULL);return true;
}
__attribute__((visibility("default"))) int app_module_init(void){
 rt=risc_runtime_get_api(1);
 if(!rt || rt->struct_size<RISC_RUNTIME_DEFAULT_REQUEST_V1_SIZE || !rt->request_default || !rt->retain_invocation)return -1;
 trace("init","begin","version=1.3.17");
 for(unsigned i=0;i<5;++i){grants[i].struct_size=sizeof(grants[i]);trace("capability-acquire","begin",cap_names[i]);if(!rt->acquire(cap_names[i],1,0,&grants[i])){fail("capability-acquire",cap_names[i]);goto error;}trace("capability-acquire","ok",cap_names[i]);}
 display=grants[0].api;navigation=grants[1].api;storage=grants[2].api;files=grants[3].api;touch=grants[4].api;
 if(!display || display->api_version!=1 || display->struct_size<sizeof(*display) || !display->get_info || !display->acquire || !display->release || !display->submit || !display->present_status ||
 !navigation || navigation->api_version!=1 || navigation->struct_size<sizeof(*navigation) || !navigation->poll || !navigation->reset ||
 !storage || storage->api_version!=1 || storage->struct_size<sizeof(*storage) || !storage->refresh || !storage->ready || !storage->dir_open || !storage->dir_next || !storage->dir_close || !storage->file_open_read || !storage->file_read || !storage->file_close ||
 !files || files->api_version!=1 || files->struct_size<sizeof(*files) || !files->source_path_get || !touch || touch->api_version!=1 || touch->struct_size<sizeof(*touch) || !touch->subscribe || !touch->unsubscribe || !touch->poll || !touch->next || !touch->snapshot){fail("capability-table","invalid ABI or missing function");goto error;}
 trace("display-init","begin",NULL);
 {risc_display_info_v1 info={0};if(!display->get_info(display->context,&info) || info.api_version!=1 || info.struct_size<sizeof(info) || !(info.supported_formats&RISC_DISPLAY_FORMAT_BIT(RISC_DISPLAY_FORMAT_MONO1))){fail("display-init","unsupported surface");goto error;}
 width=info.width>info.height?info.width:info.height;height=info.width>info.height?info.height:info.width;
 if(width<480 || height<432 || width>1024 || height>1024){fail("display-init","unsupported geometry");goto error;}}
 trace("display-init","ok",NULL);
 mono=malloc(GBEMU_FRAMEBUFFER_SIZE);if(!mono){fail("framebuffer-allocate","out of memory");goto error;}
 trace("input-start","begin",NULL);if(!navigation->reset(navigation->context)){fail("input-start","reset refused");goto error;}trace("input-start","ok",NULL);trace("touch-subscribe","begin",NULL);touch_subscription=touch->subscribe(touch->context);if(!touch_subscription){fail("touch-subscribe","refused");goto error;}trace("touch-subscribe","ok",NULL);
 cancel_touch();touch_polled=now()-8;
 {risc_touch_snapshot_v1 snapshot={0};if(!touch->snapshot(touch->context,&snapshot)){fail("touch-snapshot","unavailable");goto error;}
 (void)gb_touch_sample(&touch_state,&snapshot,now(),width,height,false);}
 trace("init","complete",NULL);return 0;
error:
 if(touch_subscription){trace("touch-unsubscribe","begin",NULL);if(!touch->unsubscribe(touch->context,touch_subscription)){hold("touch-unsubscribe");return -1;}touch_subscription=0;trace("touch-unsubscribe","ok",NULL);}
 for(unsigned i=5;i--;)if(grants[i].api){trace("capability-release","begin",cap_names[i]);if(!rt->release(&grants[i])){trace("capability-release","failed",cap_names[i]);hold("init-capability-release");return -1;}trace("capability-release","ok",cap_names[i]);}
 free(mono);mono=NULL;return -1;
}
__attribute__((visibility("default"))) void app_main(void){
 trace("entry","begin",NULL);char source[GB_PATH_MAX]={0};
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
  if(touch_input.actions&GB_TOUCH_MODE){paper=!paper;redraw=true;}
  if(playing){
   if(touch_input.actions&GB_TOUCH_ROMS){playing=false;audio_set_paused(true);cancel_touch();(void)navigation->reset(navigation->context);(void)scan();}
   if(nav.buttons&RISC_NAV_BACK){if(!back_down){back=time;back_down=true;}else if((uint32_t)(time-back)>=900){playing=false;audio_set_paused(true);cancel_touch();(void)navigation->reset(navigation->context);(void)scan();}}
   else back_down=false;
   if(playing && (int32_t)(time-next)>=0){
    if(!gbemu_run_frame(emulator,mono,GBEMU_FRAMEBUFFER_SIZE,gb_buttons(nav.buttons)|touch_input.buttons,false,NULL)){snprintf(message,sizeof(message),"%s",gbemu_get_last_error_string(emulator));playing=false;cancel_touch();redraw=true;}
    else{audio_service_frame();redraw=true;}
    next=time+17; // Bounded one-frame work; no catch-up burst after slow I/O.
   }
  }else{
   if(nav.pressed&RISC_NAV_BACK){if(!strcmp(directory,"/sd"))home=true;else{gb_parent(directory);(void)scan();}}
   int delta=(nav.pressed&(RISC_NAV_UP|RISC_NAV_LEFT))?-1:(nav.pressed&(RISC_NAV_DOWN|RISC_NAV_RIGHT))?1:0;
   if(nav.pressed&RISC_NAV_PAGE_BACK){paper=!paper;redraw=true;}if(nav.pressed&RISC_NAV_PAGE_FORWARD)delta=10;
   if(touch_input.actions&GB_TOUCH_PREV)delta=-10;if(touch_input.actions&GB_TOUCH_NEXT)delta=10;
   if(delta && catalog.count){int value=(int)selected+delta;if(value<0)value=0;if(value>=(int)catalog.count)value=(int)catalog.count-1;selected=(unsigned)value;redraw=true;}
   if(touch_input.actions&GB_TOUCH_PICK){unsigned row=selected/10*10+touch_input.row;if(row<catalog.count){selected=row;nav.pressed|=RISC_NAV_CONFIRM;}}
   if((nav.pressed&RISC_NAV_CONFIRM) && catalog.count){
    char path[GB_PATH_MAX];gb_entry* entry=&catalog.entries[selected];
    if(gb_join(directory,entry->name,path,sizeof(path))){cancel_touch();if(entry->directory){strcpy(directory,path);(void)scan();}else{(void)load_rom(path);next=now();redraw=true;}}
   }
  }
  if(redraw && !pending && !failed && !home){if(playing)draw_game();else draw_picker();redraw=false;}
  if(!failed && !home)rt->yield_ms(playing?1:10);
 }
 bool clean=cleanup();
 if(home && clean){trace("home-request","begin",NULL);if(!rt->request_default())fail("home-request","refused");else trace("home-request","accepted",NULL);}
 trace("entry","returned",failure);
}
__attribute__((visibility("default"))) void app_module_fini(void){
 if(retained)return;
 if(mono || emulator || grants[0].api)(void)cleanup();
}
