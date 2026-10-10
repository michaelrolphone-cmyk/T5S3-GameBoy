#include "platform.hpp"
#include "geometry.hpp"
#include <RiscRuntimeV1.h>
#include <RiscDisplayOutputV1.h>
#include <RiscInputNavigationV1.h>
#include <RiscTouchV1.h>
#include <RiscBatteryGaugeV1.h>
#include <RiscRealtimeV1.h>
#include <RiscKeyValueV1.h>
#include <T5FileOpenApi.h>
#include <PortableTimeZonePreference.h>
#include <PortableTimeFormat.h>
#include <PortableQuickPreferences.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "epd_video.h"
#include "touch_gt911.h"
#include "battery_power.h"
#include "night_light.h"
#include "snes_mini_controller.h"
#include "usb_hid_gamepad.h"
#include "gbemu.h"

namespace {
const risc_runtime_api_v1 *rt;
const risc_display_output_api_v1 *display;
const risc_input_navigation_api_v1 *navigation;
const risc_storage_volume_api_v1 *storage;
const risc_touch_api_v1 *touch;
const risc_battery_gauge_api_v1 *battery;
const risc_realtime_api_v1 *realtime;
const risc_key_value_v1 *preferences;
const t5_file_open_api_v1 *files;
const char *names[]={"display.output","input.navigation","storage.volume","file.open","input.touch.raw","board.battery","runtime.realtime","storage.key-value"};
risc_runtime_capability_v1 grants[8];
bool retained,failed,exit_requested,started,foreground,clean_next;
uint32_t last_ms,submitted_ms,vsync_count;
uint64_t elapsed_ms,subscription;
risc_display_present_token_v1 pending;
risc_display_surface_v1 surface;
uint8_t *buffers[2];
unsigned back;
CapGeometry geometry;
constexpr size_t frame_bytes=960u*540u/8u;
risc_input_navigation_frame_v1 nav;
touch_state_t cached_touch;
bool touch_blocked=true,touch_seen;
uint64_t touch_sequence,touch_timestamp;
uint32_t last_touch_poll;
uint16_t light=400,launch_light=400;
char first_failure[96];
char device_label[40]="RiscRTE",display_label[32];
portable_timezone_rule timezone_rule={};
unsigned time_format;
uint32_t rom_load_started;
uint32_t rom_load_attempt;
unsigned rom_frame_stage;
risc_display_present_token_v1 rom_first_token;

bool complete_present() {
  if(!cap_ready()) return false;
  if(!pending) return true;
  risc_display_present_status_v1 state={};
  if(!display->present_status(display->context,pending,&state)) { cap_hold("display-present-status");return false; }
  if(state.state==RISC_DISPLAY_PRESENT_COMPLETE || state.state==RISC_DISPLAY_PRESENT_SUPERSEDED) {
    if(rom_frame_stage==3 && pending==rom_first_token){
      char detail[96];snprintf(detail,sizeof(detail),"elapsed_ms=%lu present_state=%u",(unsigned long)(cap_millis()-rom_load_started),unsigned(state.state));
      cap_log("rom-first-playable-completion",state.state==RISC_DISPLAY_PRESENT_COMPLETE?"ok":"superseded",detail);rom_frame_stage=4;
    }
    pending=0;++vsync_count;if(vsync_count==1)cap_log("display-first-completion","ok","first visible frame");return true;
  }
  if(state.state==RISC_DISPLAY_PRESENT_FAILED) { cap_hold("display-present-failed");return false; }
  if(state.state!=RISC_DISPLAY_PRESENT_QUEUED && state.state!=RISC_DISPLAY_PRESENT_ACTIVE) { cap_hold("display-present-state");return false; }
  if(uint32_t(cap_millis()-submitted_ms)>10000u) cap_hold("display-present-timeout");
  return false;
}
bool drain_display() {
  for(unsigned n=0;pending && n<2002 && cap_ready();++n) {
    if(complete_present())break;
    if(cap_ready())rt->yield_ms(5);
  }
  if(!cap_ready())return false;
  if(pending){cap_hold("cleanup-display-pending");return false;}
  return true;
}

void poll_navigation() {
  if(!cap_ready()) return;
  risc_input_navigation_frame_v1 next={};
  if(!navigation->poll(navigation->context,&next)) { cap_fail("navigation-poll","refused");return; }
  nav=next;
  if(next.pressed&RISC_NAV_HOME) cap_exit();
}
bool cleanup() {
  if(!cap_ready())return false;
  if(!drain_display())return false;
  if(surface.frame){display->release(display->context,surface.frame);surface={};}
  if(foreground){
    if(!navigation->foreground(navigation->context,nullptr,0)){cap_hold("navigation-foreground-release");return false;}
    foreground=false;
  }
  if(subscription){
    if(!touch->unsubscribe(touch->context,subscription)){cap_hold("touch-unsubscribe");return false;}
    subscription=0;
  }
  for(unsigned i=8;i--;)if(grants[i].api){
    cap_log("capability-release","begin",names[i]);
    if(!rt->release(&grants[i])){cap_hold("capability-release");return false;}
    grants[i]={};
    cap_log("capability-release","ok",names[i]);
  }
  cap_free(buffers[0]);cap_free(buffers[1]);buffers[0]=buffers[1]=nullptr;
  return true;
}
}

bool cap_ready(){return rt && !retained;}
bool cap_retained(){return retained;}
bool cap_running(){return cap_ready() && !failed && !exit_requested;}
const risc_storage_volume_api_v1 *cap_storage(){return cap_ready()?storage:nullptr;}
void cap_log(const char *operation,const char *result,const char *detail){
  if(!cap_ready() || !rt->diagnostic)return;
  char line[240];
  snprintf(line,sizeof(line),"GAMEBOY t_ms=%lu load=%lu stage=%s result=%s %.100s",(unsigned long)cap_millis(),(unsigned long)rom_load_attempt,operation,result,detail?detail:"");
  for(unsigned i=0;line[i];++i)if((unsigned char)line[i]<32)line[i]=' ';
  (void)rt->diagnostic(line);
}
void cap_logf(const char *severity,const char *format,...){
  if(!cap_ready())return;
  // Original application counters remain available to its renderer but do not
  // flood the diagnostic ring with unchanged once-per-second telemetry.
  if(strncmp(format,"fps ",4)==0 || strncmp(format,"perf ",5)==0 || strncmp(format,"page=%u power=",14)==0)return;
  char text[160];va_list args;va_start(args,format);vsnprintf(text,sizeof(text),format,args);va_end(args);
  cap_log("application",severity,text);
}
void cap_rom_begin(const char *path){if(!cap_ready())return;++rom_load_attempt;rom_load_started=cap_millis();rom_frame_stage=0;rom_first_token=0;cap_log("selected-rom-request","begin",path);}
void cap_rom_ready(){if(!cap_ready())return;rom_frame_stage=1;cap_log("selected-rom-emulator","start",nullptr);}
void cap_rom_frame(bool rendered){if(cap_ready() && rendered && rom_frame_stage==1){rom_frame_stage=2;cap_log("rom-first-emulated-frame","ok",nullptr);}}
void cap_fail(const char *operation,const char *detail){
  if(retained)return;
  if(!first_failure[0])snprintf(first_failure,sizeof(first_failure),"%s %.48s",operation,detail?detail:"");
  failed=true;cap_log(operation,"failed",detail);
}
void cap_hold(const char *operation){
  if(retained)return;
  // A refused checked cleanup may already revoke native authority. Preserve
  // the failure in local memory, then invoke only the terminal Runtime fence.
  // No health/diagnostic/provider call is permitted after that refusal.
  if(!first_failure[0])snprintf(first_failure,sizeof(first_failure),"%s: custody retained",operation);
  failed=true;
  retained=true;
  if(rt && rt->retain_invocation)(void)rt->retain_invocation();
}
uint32_t cap_millis(){
  if(!cap_ready())return last_ms;
  risc_runtime_health_v1 health={};health.struct_size=sizeof(health);
  if(!rt->health(&health)){failed=true;return last_ms;}
  if(!started){last_ms=health.uptime_ms;started=true;}
  elapsed_ms+=uint32_t(health.uptime_ms-last_ms);last_ms=health.uptime_ms;
  return last_ms;
}
int64_t cap_micros(){(void)cap_millis();return int64_t(elapsed_ms)*1000;}
extern "C" int64_t minimal_clock_us(){return cap_micros();}
void cap_yield(){if(cap_ready()){rt->yield_ms(1);(void)complete_present();poll_navigation();}}
void cap_delay(uint32_t ms){
  uint32_t start=cap_millis();
  do { if(!cap_ready())return; rt->yield_ms(ms>5?5:ms?ms:1);(void)complete_present();poll_navigation(); }
  while(cap_running() && uint32_t(cap_millis()-start)<ms);
}
void cap_exit(){exit_requested=true;}
void *cap_alloc(size_t bytes){return cap_ready()?malloc(bytes):nullptr;}
void cap_free(void *p){if(!retained)free(p);}
extern "C" void *cap_core_calloc(size_t count,size_t size){return cap_ready()?calloc(count,size):nullptr;}
extern "C" void cap_core_free(void *p){cap_free(p);}
extern "C" void *cap_core_realloc(void *p,size_t size){return cap_ready()?realloc(p,size):nullptr;}
void paperboy_owner_call(void(*fn)(void*),void *context){if(cap_ready() && fn)fn(context);}
uint32_t cap_epoch(){
  if(!cap_ready() || !realtime)return 0;
  risc_realtime_snapshot_v1 value={};value.struct_size=sizeof(value);
  int32_t result=realtime->read(realtime->context,&value);
  if(result==RISC_REALTIME_CONTEXT){cap_hold("realtime-read-context");return 0;}
  return result==0 && value.validity==RISC_REALTIME_VALID && value.epoch_seconds>0 && value.epoch_seconds<=UINT32_MAX?uint32_t(value.epoch_seconds):0;
}
void cap_clock_text(uint32_t epoch,char *out,size_t capacity){
  portable_timezone_civil civil={};
  if(portable_timezone_utc_to_local(&timezone_rule,epoch,&civil,nullptr)!=PORTABLE_TIMEZONE_OK){snprintf(out,capacity,"--:--");return;}
  if(time_format==PORTABLE_TIME_FORMAT_24)snprintf(out,capacity,"%u:%02u",civil.hour,civil.minute);
  else snprintf(out,capacity,"%u:%02u %s",civil.hour%12?civil.hour%12:12,civil.minute,civil.hour<12?"AM":"PM");
}
const char *cap_device_label(){return device_label;}
const char *cap_display_label(){return display_label;}
bool cap_file_source(char *out,size_t capacity){return cap_ready() && files && files->source_path_get(out,capacity);}

bool cap_display_init(){return cap_ready() && buffers[0] && buffers[1];}
bool epd_video_power_on(){return cap_display_init();}
bool epd_video_start(){return cap_display_init();}
uint8_t *epd_video_get_backbuffer(){return buffers[back];}
size_t epd_video_get_backbuffer_size(){return frame_bytes;}
bool epd_video_can_submit(){return cap_running() && complete_present();}
bool epd_video_submit_pending(){return cap_ready() && !complete_present();}
uint32_t epd_video_get_vsync_count(){if(cap_ready())(void)complete_present();return vsync_count;}
bool epd_video_submit(uint16_t dirty_y,uint16_t dirty_height){
  if(!epd_video_can_submit())return false;
  surface={};
  if(!display->acquire(display->context,RISC_DISPLAY_FORMAT_MONO1,&surface)){cap_fail("display-frame-acquire","refused");return false;}
  const unsigned w=surface.width>surface.height?surface.width:surface.height;
  const unsigned h=surface.width>surface.height?surface.height:surface.width;
  if(!surface.frame || !surface.pixels || surface.pixel_format!=RISC_DISPLAY_FORMAT_MONO1 || w!=geometry.width || h!=geometry.height || surface.stride_bytes<(surface.width+7)/8 || surface.size_bytes<uint64_t(surface.stride_bytes)*surface.height){cap_fail("display-surface","unsupported geometry");return false;}
  memset(surface.pixels,0,surface.size_bytes);
  // Aspect-preserving 960x540 -> 800x450 in the physical landscape viewport.
  // The original portrait UI consequently occupies 450x800, centered on X4.
  const uint8_t *source=buffers[back];
  for(unsigned y=0;y<geometry.view_height;++y)for(unsigned x=0;x<geometry.view_width;++x){
    unsigned sx=x*960/geometry.view_width,sy=y*540/geometry.view_height;
    if(!(source[sy*120+sx/8]&(0x80u>>(sx&7))))continue;
    unsigned dx,dy;geometry.raw(x+geometry.x,y+geometry.y,dx,dy);
    static_cast<uint8_t*>(surface.pixels)[dy*surface.stride_bytes+dx/8]|=0x80u>>(dx&7);
  }
  risc_display_present_options_v1 options={uint8_t(clean_next?RISC_DISPLAY_PRESENT_CLEAN:RISC_DISPLAY_PRESENT_LOW_LATENCY),RISC_DISPLAY_QUEUE_FIFO,0};
  risc_display_present_token_v1 token=0;
  // Dirty rows are transformed into provider coordinates; surface padding stays
  // white. Full-screen redraws include it so old foreground content is cleared.
  risc_display_rect_v1 damage={};size_t count=0;
  if(dirty_height && dirty_y<540 && dirty_y+dirty_height<=540 && dirty_height<540){
    unsigned first=dirty_y*geometry.view_height/540,last=(unsigned(dirty_y+dirty_height)*geometry.view_height+539)/540;
    if(surface.height>surface.width)damage={int32_t(geometry.raw_width-geometry.y-last),int32_t(geometry.x),last-first,geometry.view_width};
    else damage={int32_t(geometry.x),int32_t(geometry.y+first),geometry.view_width,last-first};
    count=1;
  }
  if(vsync_count==0)cap_log("original-ui-first-frame","submit","low-latency, original renderer");
  if(!display->submit(display->context,surface.frame,count?&damage:nullptr,count,&options,&token)){cap_fail("display-submit","refused");return false;}
  surface={};
  if(!token){cap_hold("display-submit-token");return false;}
  if(rom_frame_stage==2){rom_first_token=token;rom_frame_stage=3;cap_log("rom-first-playable-frame","submit","low-latency");}
  pending=token;submitted_ms=cap_millis();back^=1;clean_next=false;return true;
}
void epd_video_flip(uint16_t y,uint16_t h){
  while(cap_running() && !epd_video_can_submit())cap_yield();
  if(cap_running())(void)epd_video_submit(y,h);
}
void cap_wait_display(uint8_t hold_frames){
  while(cap_running() && !complete_present())cap_yield();
  // The selected driver owns physical pulse repetition/settling. A requested
  // hold keeps the visible completed image; it never fabricates scan counts.
  if(cap_running() && hold_frames>1)cap_delay(uint32_t(hold_frames-1)*100);
}
bool cap_clean_display(){clean_next=true;return cap_running();}
void epd_video_shutdown(){if(cap_ready())cap_wait_display(1);}

bool touch_init(){return cap_ready() && subscription;}
void touch_set_rotation(int){/* Original UI owns portrait/landscape transforms. */}
void touch_debug_dump_once_per_second(){}
bool touch_read(touch_state_t *out){
  if(!out)return false;
  *out={};if(!cap_ready())return false;
  uint32_t time=cap_millis();
  if(uint32_t(time-last_touch_poll)<8){*out=cached_touch;return true;}last_touch_poll=time;
  bool ok=touch->poll(touch->context,16),drained=false;
  for(unsigned i=0;i<RISC_TOUCH_QUEUE_LENGTH;++i){risc_touch_event_v1 event={};int32_t rc=touch->next(touch->context,subscription,&event);if(!rc){drained=true;break;}if(rc!=1){ok=false;break;}if(event.kind<RISC_TOUCH_EVENT_DOWN || event.kind>RISC_TOUCH_EVENT_BUTTON_UP)ok=false;}
  risc_touch_snapshot_v1 s={};if(!touch->snapshot(touch->context,&s)){touch_blocked=true;cached_touch={};return false;}
  uint32_t ids=0;
  const bool dimensions=geometry.touch_compatible(s.width,s.height);
  if(!touch_seen || !dimensions){char detail[96];snprintf(detail,sizeof(detail),"display=%ux%u touch=%ux%u",geometry.raw_width,geometry.raw_height,unsigned(s.width),unsigned(s.height));cap_log("touch-geometry",dimensions?"compatible":"invalid",detail);}
  ok=ok && drained && dimensions && s.contact_count<=5 && !(s.buttons&~RISC_TOUCH_BUTTON_PRIMARY);
  for(unsigned i=0;i<s.contact_count && i<5;++i){auto &c=s.contacts[i];if(!c.id || c.id>16 || c.x>=s.width || c.y>=s.height || ids&(1u<<c.id))ok=false;ids|=c.id<=16?1u<<c.id:0;}
  if(touch_seen && (s.sequence<touch_sequence || s.timestamp_ms<touch_timestamp))ok=false;
  if(!touch_seen || s.sequence!=touch_sequence || s.timestamp_ms!=touch_timestamp){touch_seen=true;touch_sequence=s.sequence;touch_timestamp=s.timestamp_ms;}
  // A successful provider poll with no new report leaves its authoritative
  // snapshot unchanged. Quiet input is not a stale-sample fault.
  if(!ok)touch_blocked=true;
  if(touch_blocked){if(ok && !s.contact_count && !s.buttons)touch_blocked=false;cached_touch={};return ok;}
  cached_touch={};
  if(s.buttons&RISC_TOUCH_BUTTON_PRIMARY)cap_exit();
  for(unsigned i=0;i<s.contact_count;++i){const auto &c=s.contacts[i];unsigned px,py;if(!geometry.touch_portrait(c.x,c.y,s.width,s.height,px,py))continue;unsigned n=cached_touch.points++;cached_touch.x[n]=px;cached_touch.y[n]=py;cached_touch.id[n]=c.id;}
  cached_touch.touched=cached_touch.points!=0;*out=cached_touch;return true;
}
uint8_t snes_mini_controller_buttons(){
  poll_navigation();uint32_t n=nav.buttons;
  return uint8_t((n&RISC_NAV_CONFIRM?GBEMU_INPUT_A:0)|(n&RISC_NAV_BACK?GBEMU_INPUT_B:0)|(n&RISC_NAV_LEFT?GBEMU_INPUT_LEFT:0)|(n&RISC_NAV_RIGHT?GBEMU_INPUT_RIGHT:0)|(n&RISC_NAV_UP?GBEMU_INPUT_UP:0)|(n&RISC_NAV_DOWN?GBEMU_INPUT_DOWN:0));
}
uint8_t snes_mini_controller_navigation_buttons(){uint32_t n=nav.buttons;return uint8_t((n&RISC_NAV_CONFIRM?GBEMU_INPUT_A:0)|(n&RISC_NAV_BACK?GBEMU_INPUT_B:0)|(n&RISC_NAV_LEFT?GBEMU_INPUT_LEFT:0)|(n&RISC_NAV_RIGHT?GBEMU_INPUT_RIGHT:0)|(n&RISC_NAV_UP?GBEMU_INPUT_UP:0)|(n&RISC_NAV_DOWN?GBEMU_INPUT_DOWN:0));}
uint8_t snes_mini_controller_take_actions(){return uint8_t((nav.pressed&RISC_NAV_PAGE_BACK?SNES_ACTION_SETTINGS:0)|(nav.pressed&RISC_NAV_PAGE_FORWARD?SNES_ACTION_ROTATE:0));}
UsbGamepadTestStatus usb_hid_gamepad_test_status(){UsbGamepadTestStatus s={};strcpy(s.stage,"CAPABILITY INPUT");strcpy(s.error,"USB HOST GAMEPAD NOT BOUND");s.buttons=nav.buttons;return s;}
void usb_hid_gamepad_test_active(bool){}
bool battery_begin(){return cap_ready() && battery;}
void battery_service(){}
bool battery_read_status(PaperboyBatteryStatus &out){
  out={};if(!cap_ready() || !battery)return false;risc_battery_sample_v1 s={};
  if(!battery->read(battery->context,&s))return false;
  out.gauge_found=out.gauge_read_ok=true;out.soc_percent=s.percent;out.voltage_mv=s.millivolts;out.charging=s.charging!=0;out.low_battery=s.percent<=10;return true;
}
void night_light_init(){/* The launch level is read once from persisted shell settings. */}
void night_light_set_launch_level(uint8_t p){launch_light=light=uint16_t(p)*10;}
uint16_t night_light_brightness_tenths(){return light;}
uint8_t night_light_brightness(){return uint8_t((light+5)/10);}
bool night_light_set_brightness_tenths(uint16_t value){if(!cap_ready() || value>1000 || !display->set_brightness || !display->set_brightness(display->context,value,1000))return false;light=value;return true;}
bool night_light_set_brightness(uint8_t value){return value<=100 && night_light_set_brightness_tenths(uint16_t(value)*10);}
bool night_light_adjust_brightness(bool brighter){int next=light;if(brighter)next+=light<10?1:10;else next-=light<=10?1:10;if(next<0)next=0;if(next>1000)next=1000;return night_light_set_brightness_tenths(uint16_t(next));}
void night_light_shutdown(){if(cap_ready() && light!=launch_light && !night_light_set_brightness_tenths(launch_light))cap_fail("backlight-restore","refused");}

extern "C" __attribute__((visibility("default"))) int app_module_init(){
  rt=risc_runtime_get_api(1);
  if(!rt || rt->struct_size<RISC_RUNTIME_DEFAULT_REQUEST_V1_SIZE || !rt->health || !rt->yield_ms || !rt->acquire || !rt->release || !rt->retain_invocation || !rt->request_default)return -1;
  return 0;
}
static int begin_application(){
  cap_log("init","begin","original-ui 1.3.22");
  for(unsigned i=0;i<8;++i){grants[i].struct_size=sizeof(grants[i]);cap_log("capability-acquire","begin",names[i]);if(!rt->acquire(names[i],1,0,&grants[i])){cap_fail("capability-acquire",names[i]);cleanup();return -1;}cap_log("capability-acquire","ok",names[i]);}
  display=static_cast<const risc_display_output_api_v1*>(grants[0].api);navigation=static_cast<const risc_input_navigation_api_v1*>(grants[1].api);storage=static_cast<const risc_storage_volume_api_v1*>(grants[2].api);files=static_cast<const t5_file_open_api_v1*>(grants[3].api);touch=static_cast<const risc_touch_api_v1*>(grants[4].api);battery=static_cast<const risc_battery_gauge_api_v1*>(grants[5].api);realtime=static_cast<const risc_realtime_api_v1*>(grants[6].api);preferences=static_cast<const risc_key_value_v1*>(grants[7].api);
#define TABLE(p) ((p) && (p)->api_version==1 && (p)->struct_size>=sizeof(*(p)))
  if(!TABLE(display) || !display->get_info || !display->acquire || !display->release || !display->submit || !display->present_status || !TABLE(navigation) || !navigation->poll || !navigation->reset || !navigation->foreground || !TABLE(storage) || !TABLE(files) || !files->source_path_get || !TABLE(touch) || !touch->poll || !touch->next || !touch->snapshot || !touch->subscribe || !touch->unsubscribe || !TABLE(battery) || !battery->read || !TABLE(realtime) || !realtime->read || !TABLE(preferences) || !preferences->get){cap_fail("capability-table","invalid ABI");cleanup();return -1;}
#undef TABLE
  risc_display_info_v1 info={};if(!display->get_info(display->context,&info) || !geometry.configure(info.width,info.height) || !(info.supported_formats&RISC_DISPLAY_FORMAT_BIT(RISC_DISPLAY_FORMAT_MONO1))){cap_fail("display-info","X4 geometry unavailable");cleanup();return -1;}
  snprintf(display_label,sizeof(display_label),"%ux%u E-PAPER",unsigned(info.width),unsigned(info.height));
  risc_runtime_health_v1 health={};health.struct_size=sizeof(health);
  if(rt->health(&health))snprintf(device_label,sizeof(device_label),"%.39s",health.target);
  char timezone_id[PORTABLE_TIMEZONE_ID_BYTES];
  (void)portable_timezone_preference_load(preferences,timezone_id);
  (void)portable_timezone_resolve(timezone_id,sizeof(timezone_id),&timezone_rule);
  (void)portable_time_format_load(preferences,&time_format);
  unsigned level=40;if(pqa_preference_load(preferences,PQA_BRIGHTNESS_KEY,40,0,&level))launch_light=light=uint16_t(level)*10;
  buffers[0]=static_cast<uint8_t*>(cap_alloc(frame_bytes));buffers[1]=static_cast<uint8_t*>(cap_alloc(frame_bytes));
  if(!buffers[0] || !buffers[1]){cap_fail("display-buffers","allocation failed");cleanup();return -1;}memset(buffers[0],0,frame_bytes);memset(buffers[1],0,frame_bytes);
  if(!navigation->reset(navigation->context)){cap_fail("input-reset","refused");cleanup();return -1;}
  risc_input_foreground_v1 claim={"input.touch.raw",1};if(!navigation->foreground(navigation->context,&claim,1)){cap_hold("input-foreground");return -1;}foreground=true;
  subscription=touch->subscribe(touch->context);if(!subscription){cap_fail("touch-subscribe","refused");cleanup();return -1;}
  last_touch_poll=cap_millis()-8;touch_state_t initial={};if(!touch_read(&initial)){cap_fail("touch-snapshot","unavailable");cleanup();return -1;}
  paperboy_storage_bind_host();cap_log("init","complete",nullptr);return 0;
}
extern "C" __attribute__((visibility("default"))) void app_main(){
  cap_log("entry","begin","original application/controller");
  if(begin_application()!=0)return;
  cap_console_setup();
  if(retained)return;
  if(!drain_display())return;
  cap_console_cleanup();
  if(cleanup() && exit_requested){cap_log("home-request","begin",nullptr);if(!rt->request_default())cap_fail("home-request","refused");}
  cap_log("entry","returned",first_failure);
}
extern "C" __attribute__((visibility("default"))) void app_module_fini(){if(!retained && grants[0].api && drain_display()){cap_console_cleanup();(void)cleanup();}}
