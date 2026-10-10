#include "platform.hpp"
#include "geometry.hpp"
#include "paperboy_ui.h"
#include "mono_canvas.h"
#include "builtin_demo_rom.h"
#include "audio.h"
#include "epd_video.h"
#include <RiscRuntimeV1.h>
#include <RiscDisplayOutputV1.h>
#include <RiscInputNavigationV1.h>
#include <RiscTouchV1.h>
#include <RiscBatteryGaugeV1.h>
#include <RiscRealtimeV1.h>
#include <RiscKeyValueV1.h>
#include <T5FileOpenApi.h>
#include "../../tests/capability_storage_fixture.hpp"
#include <cstdarg>
#ifdef GAMEBOY_REAL_RUNTIME
#include <fstream>
extern bool test_run_native_phase(const char*);
extern "C" int gameboy_module_init();
extern "C" void gameboy_main();
extern "C" void gameboy_module_fini();
extern "C" const risc_runtime_api_v1 *gameboy_runtime_get_api(uint32_t);
#endif

namespace f = capability_storage_fixture;
extern "C" int app_module_init();
extern "C" void app_main();
extern "C" void app_module_fini();
extern "C" void lifecycle_normalize_state(void *,size_t);
#ifdef GAMEBOY_REAL_GT911
extern "C" const risc_touch_api_v1 *hid_watch_touch_start();
extern "C" void hid_watch_touch_stop();
#endif
namespace {
std::string scenario, output;
std::set<void *> app_memory;
std::set<unsigned> grants;
std::vector<std::string> events, diagnostics;
bool header_case(){return scenario.rfind("header-",0)==0;}
bool header_memory(){return scenario=="header-load-memory" || scenario=="header-save-close-retained";}
bool header_prepared=false;
bool mailbox_case(){return scenario.rfind("mailbox-",0)==0;}
bool timing_case(){return scenario.rfind("timing-",0)==0;}
bool compute_case(){return scenario.find("compute")!=std::string::npos;}
bool slow_case(){return scenario.find("slow")!=std::string::npos;}
bool rom_failure_case(){return scenario=="rom-read-failure" || scenario=="rom-open-failure" || scenario=="rom-init-failure";}
std::vector<uint8_t> pixels(480*800/8);
risc_runtime_api_v1 runtime{};
#ifdef GAMEBOY_REAL_RUNTIME
risc_runtime_capability_v1 real_time_grant{};
#endif
risc_display_output_api_v1 display{};
risc_input_navigation_api_v1 navigation{};
risc_touch_api_v1 touch{};
risc_battery_gauge_api_v1 battery{};
risc_realtime_api_v1 realtime{};
risc_key_value_v1 kv{};
t5_file_open_api_v1 file_open{};
uint32_t now = 1000, phase_started = 0, previous_nav = 0;
unsigned phase = 0, calls = 0, frame_count = 0, game_frames = 0;
unsigned library_frames = 0, settings_frames = 0, game_presents = 0;
unsigned saved = 0, loaded = 0, home_requests = 0, retains = 0, released = 0;
unsigned status_calls = 0, error_pending_polls = 0, memory_after_terminal = 0;
bool terminal = false, source_taken = false, focus = false, subscribed = false;
bool acquired = false, pending = false, input_failed = false;
bool entry_running = false;
unsigned display_width=480,display_height=800;
uint64_t token = 0; uint32_t complete_at = 0;
PaperboyPage view = PaperboyPage::SdCard;
CapGeometry geometry;
std::vector<uint8_t> newest_game,final_simulation,submitted_pixels;
std::vector<unsigned> simulation_times,simulation_inputs,simulation_hashes,presentation_frames;
unsigned audio_frames=0,newest_game_frame=0,freshness_checks=0;
bool mailbox_exercised=false;
gbemu_t *last_emu=nullptr;
uint32_t hash_bytes(const uint8_t *data,size_t size){uint32_t value=2166136261u;for(size_t i=0;i<size;++i)value=(value^data[i])*16777619u;return value;}
void check_newest_game(){
  if(!timing_case() || newest_game.empty())return;
  unsigned checked=0;
  for(unsigned y=0;y<geometry.view_height;++y)for(unsigned x=0;x<geometry.view_width;++x){
    const unsigned sx=x*960/geometry.view_width,sy=y*540/geometry.view_height;
    const unsigned gx=539-sy-PAPERBOY_GAME_X,gy=sx-PAPERBOY_GAME_Y;
    if(gx>=GBEMU_FRAME_WIDTH || gy>=GBEMU_FRAME_HEIGHT)continue;
    unsigned rx,ry;geometry.raw(x+geometry.x,y+geometry.y,rx,ry);
    const bool actual=(pixels[ry*(display_width/8)+rx/8]&(0x80u>>(rx&7)))!=0;
    const bool expected=(newest_game[gy*GBEMU_FRAME_PITCH_BYTES+gx/8]&(0x80u>>(gx&7)))==0;
    assert(actual==expected);++checked;
  }
  assert(checked>10000);++freshness_checks;presentation_frames.push_back(newest_game_frame);
}

void call(const char *name) {
  if (terminal) { std::fprintf(stderr,"CALL AFTER TERMINAL: %s\n",name); std::abort(); }
  ++calls; events.emplace_back(name);
  if (now > 60000) { std::fprintf(stderr,"TIMEOUT phase=%u frames=%u game=%u view=%u\n",phase,frame_count,game_frames,unsigned(view)); std::abort(); }
}
void next_phase(unsigned value) { phase=value;phase_started=now;std::printf("phase=%u t=%u\n",phase,now); }
void drive() {
  if(header_case()){
    if(phase==0 && game_frames>=3 && now>phase_started+150)next_phase(header_memory()?3:5);
    if(phase==4 && saved && now>phase_started+150)next_phase(5);
    if(phase==6 && now>phase_started+250)next_phase(9);
    return;
  }
  if(timing_case()){if(now>=11000 && phase!=9)next_phase(9);return;}
  if (scenario != "journey" && scenario != "slow-journey" && scenario != "file-handoff" && scenario != "save-terminal" && scenario != "launch-terminal" && !rom_failure_case() && scenario!="rom-retry") return;
  if (phase==0 && frame_count && now>phase_started+150) next_phase(scenario=="file-handoff"?2:1);
  if (phase==10 && now>phase_started+150)next_phase(1);
  if (phase==1 && view==PaperboyPage::Game) next_phase(2);
  if (phase==2 && game_frames>=3 && now>phase_started+150) next_phase(3);
  if (phase==4 && saved && now>phase_started+150) next_phase(5);
  if (phase==6 && loaded && now>phase_started+150) next_phase(7);
  if (phase==8 && settings_frames && now>phase_started+150) next_phase(9);
}
void capture(const char *name) {
  const std::string path=output+"/"+name+".pbm";
  FILE *file=std::fopen(path.c_str(),"wb");assert(file);
  std::fprintf(file,"P4\n%u %u\n",display_width,display_height);assert(std::fwrite(pixels.data(),1,pixels.size(),file)==pixels.size());std::fclose(file);
}
bool health(risc_runtime_health_v1 *out) { call("health");if(!timing_case() && !mailbox_case())++now;out->uptime_ms=now;std::strcpy(out->target,"X4 host lifecycle fixture");return true; }
void yield(uint32_t ms) { call("yield");now+=ms?ms:1;drive(); }
bool diagnostic(const char *line) {
  call("diagnostic");
  diagnostics.emplace_back(line);
  std::printf("%s\n",line);
  if(std::strstr(line,"stage=selected-rom-load result=failed")){
    if(rom_failure_case())next_phase(9);
    else if(scenario=="rom-retry"){f::fail_read=false;next_phase(10);}
  }
  if (std::strstr(line,"game session saved")) ++saved;
  if (std::strstr(line,"game state restored")) ++loaded;
  return true;
}
bool retain() { assert(!terminal);terminal=true;++retains;return true; }
bool request_home() { call("request_default");assert(grants.empty() && !pending && !acquired && !focus && !subscribed);++home_requests;return true; }
bool acquire_cap(const char *name,uint32_t version,uint64_t instance,risc_runtime_capability_v1 *out) {
  call("acquire");assert(version==1 && instance==0);
#ifndef GAMEBOY_REAL_RUNTIME
  if(!std::strcmp(name,"runtime.realtime") && !entry_running)return false;
#endif
  const char *names[]={"display.output","input.navigation","storage.volume","file.open","input.touch.raw","board.battery","runtime.realtime","storage.key-value"};
  const void *apis[]={&display,&navigation,&f::api.base,&file_open,&touch,&battery,&realtime,&kv};
  for(unsigned i=0;i<8;++i)if(std::strcmp(name,names[i])==0){
#ifdef GAMEBOY_REAL_RUNTIME
    if(i==6){real_time_grant.struct_size=sizeof(real_time_grant);if(!risc_runtime_get_api(1)->acquire(name,version,instance,&real_time_grant))return false;apis[i]=real_time_grant.api;}
#endif
    out->slot=i+1;out->generation=1;out->api=apis[i];grants.insert(i+1);return true;}return false;
}
bool release_cap(risc_runtime_capability_v1 *grant) {
#ifdef GAMEBOY_REAL_RUNTIME
  if(grant->slot==7)assert(risc_runtime_get_api(1)->release(&real_time_grant));
#endif
  call("release");assert(!pending && !acquired && !focus && !subscribed);assert(f::handles.empty() && f::dirs.empty());assert(grants.erase(grant->slot)==1);++released;return true;
}
bool display_info(void *,risc_display_info_v1 *out) {
  call("display_info");out->width=display_width;out->height=display_height;out->supported_formats=RISC_DISPLAY_FORMAT_BIT(RISC_DISPLAY_FORMAT_MONO1);return true;
}
bool display_acquire(void *,uint32_t format,risc_display_surface_v1 *out) {
  call("display_acquire");assert(!acquired && !pending && format==RISC_DISPLAY_FORMAT_MONO1);acquired=true;
  *out={1,pixels.data(),display_width,display_height,display_width/8,uint32_t(pixels.size()),RISC_DISPLAY_FORMAT_MONO1};return true;
}
void display_release(void *,uint64_t frame) { call("display_release");assert(acquired && frame==1);acquired=false; }
bool display_submit(void *,uint64_t frame,const risc_display_rect_v1 *damage,size_t count,const risc_display_present_options_v1 *options,uint64_t *out) {
  call("display_submit");assert(acquired && frame==1 && !pending);assert(count<=1 && options && options->queue_policy==RISC_DISPLAY_QUEUE_FIFO);
  if(count){assert(damage && damage->x>=0 && damage->y>=0 && damage->width && damage->height && unsigned(damage->x)+damage->width<=display_width && unsigned(damage->y)+damage->height<=display_height);}
  acquired=false;pending=true;*out=++token;++frame_count;
  complete_at=now+((mailbox_case() || scenario=="slow-journey" || scenario=="header-load-display-retained")?2300:timing_case()?(slow_case()?2300:scenario=="timing-lcd"?17:1):scenario=="input-error-pending"?80:5);
  if(mailbox_case() && frame_count==2){
    assert(scenario=="mailbox-latest" && count==1);
    const unsigned first=20*geometry.view_height/540,last=(49*geometry.view_height+539)/540;
    assert(damage->x==int(geometry.raw_width-geometry.y-last) && damage->y==int(geometry.x));
    assert(damage->width==last-first && damage->height==geometry.view_width);
    for(unsigned y=0;y<geometry.view_height;++y)for(unsigned x=0;x<geometry.view_width;++x){
      unsigned rx,ry;geometry.raw(x+geometry.x,y+geometry.y,rx,ry);
      const unsigned sx=x*960/geometry.view_width;
      assert(bool(pixels[ry*(display_width/8)+rx/8]&(0x80u>>(rx&7)))==bool(20u&(0x80u>>(sx&7))));
    }
  }
  check_newest_game();submitted_pixels=pixels;
  if(view==PaperboyPage::SdCard){if(!library_frames++)capture("library");}
  if(view==PaperboyPage::Settings){if(!settings_frames++)capture("settings");}
  if(view==PaperboyPage::Game){if(!game_presents++)capture("game");if(game_frames>=3)capture("game-play");}
  return true;
}
bool present_status(void *,uint64_t value,risc_display_present_status_v1 *out) {
  call("present_status");assert(value==token);++status_calls;
  if(timing_case() && pending)assert(pixels==submitted_pixels);
  if((scenario=="header-load-display-retained" && phase==6) || scenario=="display-terminal" || (scenario=="display-terminal-vsync" && status_calls==2) || (scenario=="display-terminal-ready" && status_calls==3)){return false;}
  if(pending && now>=complete_at){pending=false;out->state=RISC_DISPLAY_PRESENT_COMPLETE;}
  else out->state=pending?RISC_DISPLAY_PRESENT_ACTIVE:RISC_DISPLAY_PRESENT_COMPLETE;
  if(input_failed && pending)++error_pending_polls;
  return true;
}
bool brightness(void *,uint16_t,uint16_t){call("brightness");return true;}
bool nav_poll(void *,risc_input_navigation_frame_v1 *out) {
  call("nav_poll");drive();
  if(scenario=="input-error-pending" && pending){input_failed=true;return false;}
  uint32_t buttons=0;
  if(timing_case() && phase!=9){
    static const uint32_t sequence[]={RISC_NAV_RIGHT,RISC_NAV_CONFIRM,0,RISC_NAV_DOWN,RISC_NAV_LEFT,RISC_NAV_BACK,RISC_NAV_UP,0};
    buttons=sequence[((now-1000)/137)%8];
  }
  if(phase==1 && now-phase_started<100){buttons=RISC_NAV_CONFIRM;if(scenario=="launch-terminal")f::fail_file_close=true;
    if(scenario=="rom-read-failure" || (scenario=="rom-retry" && diagnostics.end()==std::find_if(diagnostics.begin(),diagnostics.end(),[](const std::string &line){return line.find("stage=selected-rom-load result=failed")!=std::string::npos;})))f::fail_read=true;
    if(scenario=="rom-open-failure")f::files.erase("/Test.gb");
    if(scenario=="rom-init-failure")f::files["/Test.gb"][0x147]=0xff;}
  if(phase==9)buttons=RISC_NAV_HOME;
  *out={buttons,buttons&~previous_nav,previous_nav&~buttons};previous_nav=buttons;return true;
}
bool nav_focus(void *,const risc_input_foreground_v1 *claims,size_t count) {
  call("nav_focus");if(count){assert(count==1 && claims && !focus);focus=true;}else{assert(focus);focus=false;}return true;
}
bool nav_reset(void *){call("nav_reset");return true;}
uint64_t touch_subscribe(void *){call("touch_subscribe");assert(!subscribed);subscribed=true;return 1;}
bool touch_unsubscribe(void *,uint64_t subscription){call("touch_unsubscribe");assert(subscribed && subscription==1);if(scenario=="touch-release-terminal")return false;subscribed=false;return true;}
bool touch_poll(void *,size_t){call("touch_poll");return true;}
int32_t touch_next(void *,uint64_t subscription,risc_touch_event_v1 *){call("touch_next");assert(subscription==1);return 0;}
bool touch_snapshot(void *,risc_touch_snapshot_v1 *out) {
  call("touch_snapshot");drive();*out={};out->width=480;out->height=800;out->sequence=now;out->timestamp_ms=now;
  unsigned px=0,py=0;
  if(phase==3){px=360;py=40;}
  if(phase==5){px=470;py=40;}
  if(phase==7){px=440;py=910;}
  if(px){out->contact_count=1;out->contacts[0].id=1;
    // Invert the actual integer pixel transform by finding a point whose
    // logical coordinates fall inside the requested original UI control.
    bool found=false;
    for(unsigned y=0;y<800 && !found;++y)for(unsigned x=0;x<480;++x){unsigned a,b;if(geometry.touch_portrait(x,y,480,800,a,b) && a>=px && a<=px+2 && b>=py && b<=py+2){out->contacts[0].x=x;out->contacts[0].y=y;found=true;break;}}
    assert(found);
  }
  return true;
}
bool battery_read(void *,risc_battery_sample_v1 *out){call("battery");*out={3980,75,0};return true;}
int32_t realtime_read(void *,risc_realtime_snapshot_v1 *out){call("realtime");if(scenario=="realtime-terminal")return RISC_REALTIME_CONTEXT;out->validity=RISC_REALTIME_VALID;out->epoch_seconds=1791536400;return 0;}
int32_t key_get(void *,const char *,void *,uint32_t,uint32_t *size){call("kv_get");*size=0;return RISC_KEY_VALUE_NOT_FOUND;}
bool source_get(char *out,size_t capacity){call("file_source");source_taken=true;if(!header_case() && scenario!="file-handoff" && scenario!="file-handoff-terminal" && !timing_case() && !mailbox_case() && scenario.rfind("display-terminal-",0)!=0)return false;assert(capacity>std::strlen("/sd/Test.gb"));std::strcpy(out,"/sd/Test.gb");return true;}
}
#ifdef GAMEBOY_REAL_GT911
extern "C" void hid_renderer_watch_report(risc_touch_snapshot_v1 *sample){assert(touch_snapshot(nullptr,sample));}
extern "C" uint64_t hid_renderer_watch_millis(){return now;}
#endif

extern "C" void *lifecycle_malloc(size_t n) noexcept {if(terminal){++memory_after_terminal;std::fprintf(stderr,"MALLOC AFTER TERMINAL\n");std::abort();}void *p=std::malloc(n);if(p)app_memory.insert(p);return p;}
extern "C" void *lifecycle_calloc(size_t n,size_t size) noexcept {if(terminal){++memory_after_terminal;std::abort();}void *p=std::calloc(n,size);if(p)app_memory.insert(p);return p;}
extern "C" void *lifecycle_realloc(void *p,size_t n) noexcept {if(terminal){++memory_after_terminal;std::abort();}if(p)assert(app_memory.count(p));void *q=std::realloc(p,n);if(q){app_memory.erase(p);app_memory.insert(q);}return q;}
extern "C" void lifecycle_free(void *p) noexcept {if(terminal){++memory_after_terminal;std::fprintf(stderr,"FREE AFTER TERMINAL\n");std::abort();}if(p)assert(app_memory.erase(p)==1);std::free(p);}
extern "C" bool lifecycle_run_frame(gbemu_t *emu,uint8_t *frame,size_t size,uint8_t input,bool skip,gbemu_frame_stats_t *stats){
  assert(!terminal);
  if(timing_case()){simulation_times.push_back(now);simulation_inputs.push_back(input);if(compute_case())now+=30;}
  const bool result=gbemu_run_frame(emu,frame,size,input,skip,stats);
  if(result){++game_frames;last_emu=emu;
    if(header_case() && !header_prepared){
      header_prepared=true;
      if(!header_memory() && scenario!="header-load-missing"){
        std::vector<uint8_t> state(gbemu_get_state_size(emu));assert(gbemu_save_state(emu,state.data(),state.size()));
        if(scenario=="header-load-corrupt")state[0]^=0xff;
        f::add_file("/System/State/Applications/gameboy/Test.gb.state",state.size());f::files["/System/State/Applications/gameboy/Test.gb.state"]=state;
      }
    }
    if(timing_case() && !skip){assert(frame && size==GBEMU_FRAMEBUFFER_SIZE);newest_game.assign(frame,frame+size);newest_game_frame=game_frames;}}
  return result;
}
extern "C" void lifecycle_audio_service_frame(){
  audio_service_frame();
  if(timing_case()){
    ++audio_frames;assert(last_emu);final_simulation.resize(gbemu_get_state_size(last_emu));
    assert(gbemu_save_state(last_emu,final_simulation.data(),final_simulation.size()));
    lifecycle_normalize_state(final_simulation.data(),final_simulation.size());
    simulation_hashes.push_back(hash_bytes(final_simulation.data(),final_simulation.size()));
  }
}
uint32_t lifecycle_map_actions(const touch_state_t *state,PaperboyPage page){assert(!terminal);view=page;
  if(mailbox_case() && !mailbox_exercised){
    mailbox_exercised=true;assert(pending && frame_count==1);
    const auto original_pixels=pixels;const uint32_t started_at=now;
    for(unsigned i=1;i<=20;++i){
      std::memset(epd_video_get_backbuffer(),int(i),epd_video_get_backbuffer_size());
      assert(epd_video_submit(uint16_t(19+i),10));
      assert(frame_count==1 && pending && pixels==original_pixels && now==started_at);
    }
    std::memset(epd_video_get_backbuffer(),0xfe,epd_video_get_backbuffer_size());
    if(scenario=="mailbox-latest"){
      now=complete_at;cap_service_display();assert(frame_count==2 && pending);
    }
    cap_exit();return 0;
  }
const uint32_t actions=paperboy_ui_map_actions(state,page);
  if(header_case() && (actions&PAPERBOY_ACTION_LOAD)){
    if(scenario=="header-load-close-retained")f::fail_file_close=true;
    if(scenario=="header-load-read-failure")f::fail_read=true;
    if(scenario=="header-load-read-retained"){f::terminal_at=f::calls+15;f::on_terminal=[](){cap_hold("header-load-provider");};}
  }
  if(scenario=="header-save-close-retained" && (actions&PAPERBOY_ACTION_SAVE))f::fail_file_close=true;if(actions&PAPERBOY_ACTION_SAVE){if(scenario=="slow-journey")assert(pending && frame_count==1);next_phase(4);if(scenario=="save-terminal")f::fail_file_close=true;}if(actions&PAPERBOY_ACTION_LOAD)next_phase(6);if(actions&PAPERBOY_ACTION_SETTINGS)next_phase(8);return actions;}
void lifecycle_draw_page(uint8_t *buffer,PaperboyPage page,const PaperboyBatteryStatus *status,const char *version,const char *title,bool touch_ready,const PaperboyRomLibraryView *library,const UsbGamepadTestStatus *gamepad){assert(!terminal);view=page;paperboy_ui_draw_page(buffer,page,status,version,title,touch_ready,library,gamepad);}
extern "C" void lifecycle_clear(uint8_t *buffer,size_t size,bool white){assert(!terminal);mono_clear(buffer,size,white);}
#ifdef GAMEBOY_REAL_RUNTIME
extern "C" const risc_runtime_api_v1 *gameboy_runtime_get_api(uint32_t version){assert(version==1);return &runtime;}
extern "C" int32_t test_native_time_read(void* context,risc_realtime_snapshot_v1* out){return realtime_read(context,out);}
extern "C" bool test_native_health(risc_runtime_health_v1* out){return health(out);}
extern "C" void test_native_delay(uint32_t ms){yield(ms);}
extern "C" int test_gameboy_init(){
  // Production Runtime must reject this native service before entry.
  risc_runtime_capability_v1 probe{sizeof(probe)};
  assert(!risc_runtime_get_api(1)->acquire("runtime.realtime",1,0,&probe));
  const int result=gameboy_module_init();assert(grants.empty() && calls==0);return result;
}
extern "C" void test_gameboy_main(){entry_running=true;gameboy_main();entry_running=false;}
extern "C" void test_gameboy_fini(){gameboy_module_fini();}
#else
extern "C" const risc_runtime_api_v1 *risc_runtime_get_api(uint32_t version){assert(version==1);return &runtime;}
#endif
int main(int argc,char **argv){
  assert(argc==3);scenario=argv[1];output=argv[2];if(std::getenv("GAMEBOY_NATIVE_PANEL")){display_width=800;display_height=480;}assert(geometry.configure(display_width,display_height));
  f::setup();f::admitted=cap_ready;
  f::add_file("/Test.gb",builtin_demo_rom_size());f::files["/Test.gb"]={builtin_demo_rom_data(),builtin_demo_rom_data()+builtin_demo_rom_size()};
  // Force the scanner to own a heap-backed first-level folder list before a
  // checked-close refusal, exercising the entire staged app's terminal path.
  if(scenario=="storage-terminal"){f::directories.insert("/games");f::fail_dir_close=true;}
  if(scenario=="config-terminal"){
    const std::string config="audio_engine=0\n";f::files["/paperboy.cfg"]={config.begin(),config.end()};f::fail_file_close=true;
  }
  if(scenario=="file-handoff-terminal")f::fail_file_close=true;
  runtime.api_version=1;runtime.struct_size=sizeof(runtime);runtime.health=health;runtime.yield_ms=yield;runtime.diagnostic=diagnostic;runtime.acquire=acquire_cap;runtime.release=release_cap;runtime.retain_invocation=retain;runtime.request_default=request_home;
  display.api_version=1;display.struct_size=sizeof(display);display.get_info=display_info;display.acquire=display_acquire;display.release=display_release;display.submit=display_submit;display.present_status=present_status;display.set_brightness=brightness;
  navigation={1,sizeof(navigation),nullptr,nav_poll,nav_focus,nav_reset};
  touch={1,sizeof(touch),nullptr,touch_subscribe,touch_unsubscribe,touch_poll,touch_next,touch_snapshot};
#ifdef GAMEBOY_REAL_GT911
  touch=*hid_watch_touch_start();
#endif
  battery={1,sizeof(battery),nullptr,battery_read};realtime={1,sizeof(realtime),nullptr,realtime_read};kv={1,sizeof(kv),nullptr,key_get,nullptr};
  file_open.api_version=1;file_open.struct_size=sizeof(file_open);file_open.source_path_get=source_get;
  if(header_case() || scenario=="file-handoff" || timing_case() || mailbox_case() || scenario.rfind("display-terminal-",0)==0)view=PaperboyPage::Game;
  phase_started=now;
#ifdef GAMEBOY_REAL_RUNTIME
  assert(scenario=="journey");
  std::ofstream(output+"/board.json")<<R"({"schema":"riscrte.board-hardware","schema_version":1,"board_id":"test","revision":"unspecified","buses":[],"devices":[]})";
  std::ofstream(output+"/app.json")<<R"({"type":"application","id":"gameboy-phase","version":"1.3.19","architecture":"xtensa-esp32s3","file_name":"gameboy.elf","entry":"app_main","requires":[{"capability":"runtime.realtime","api":1}]})";
  std::ofstream(output+"/boot.json")<<R"({"board":"board.json","default_app":"gameboy.elf","drivers":[],"app_capabilities":[{"manifest":"app.json","grants":[{"capability":"runtime.realtime","api":1,"instance_id":0}]}]})";
  assert(test_run_native_phase(output.c_str()));
#else
  assert(app_module_init()==0);assert(grants.empty() && calls==0);
  if(scenario=="touch-release-terminal")next_phase(9);
  entry_running=true;app_main();entry_running=false;
#endif
  const unsigned terminal_calls=calls,terminal_storage_calls=f::calls;
#ifndef GAMEBOY_REAL_RUNTIME
  app_module_fini();
#endif
  if(terminal){assert(retains==1 && memory_after_terminal==0 && calls==terminal_calls && f::calls==terminal_storage_calls && released==0 && home_requests==0);}
  else {
    assert(grants.empty() && released==8 && !focus && !subscribed && !pending && !acquired && f::handles.empty() && f::dirs.empty() && app_memory.empty());
#ifdef GAMEBOY_REAL_GT911
    hid_watch_touch_stop();
#endif
    if(scenario=="input-error-pending")assert(input_failed && error_pending_polls && home_requests==0);
    else if(header_case()){
      assert(header_prepared && home_requests==1);
      const bool expected_load=scenario=="header-load-disk" || scenario=="header-load-memory";
      assert(loaded==unsigned(expected_load));
      assert(saved==unsigned(scenario=="header-load-memory"));
    }
    else if(mailbox_case()){assert(mailbox_exercised && !saved && !loaded && home_requests==1);assert(frame_count==(scenario=="mailbox-latest"?2:1));}
    else if(timing_case()){
      assert(home_requests==1 && !saved && !loaded && audio_frames==game_frames);
      assert(game_frames>=(compute_case()?300:596) && game_frames<=(compute_case()?330:600));
      assert(freshness_checks>=(slow_case()?4:100));
      if(slow_case())assert(presentation_frames.back()>presentation_frames.front()+400 || compute_case());
      FILE *trace=std::fopen((output+"/simulation.json").c_str(),"wb");assert(trace);
      std::fprintf(trace,"{\"frames\":%u,\"audio_frames\":%u,\"freshness_checks\":%u,\"presents\":%u,\"steps\":[",game_frames,audio_frames,freshness_checks,frame_count);
      for(size_t i=0;i<simulation_times.size();++i)std::fprintf(trace,"%s[%u,%u,%u]",i?",":"",simulation_times[i],simulation_inputs[i],simulation_hashes[i]);
      std::fprintf(trace,"],\"presented_frames\":[");
      for(size_t i=0;i<presentation_frames.size();++i)std::fprintf(trace,"%s%u",i?",":"",presentation_frames[i]);
      std::fprintf(trace,"]}\n");std::fclose(trace);
      FILE *state=std::fopen((output+"/final-state.bin").c_str(),"wb");assert(state);assert(std::fwrite(final_simulation.data(),1,final_simulation.size(),state)==final_simulation.size());std::fclose(state);
      std::printf("TIMING simulated=%u audio=%u physical=%u fresh=%u last_visual=%u injected_compute_ms=%u\n",game_frames,audio_frames,frame_count,freshness_checks,newest_game_frame,compute_case()?30:0);
    }
    else if(rom_failure_case()){assert(home_requests==1 && !saved && !loaded);}
    else {assert(source_taken && game_frames>=3 && saved==1 && loaded==1 && settings_frames && home_requests==1);assert(f::files.count("/System/State/Applications/gameboy/Test.gb.state"));if(scenario=="journey" || scenario=="slow-journey" || scenario=="rom-retry")assert(library_frames);else assert(library_frames==0);}
  }
  // Every operation log is bounded, carries one attempt ID and a monotonic
  // timestamp. Successful spans cannot accidentally use a failure code.
  unsigned last_time=0,last_attempt=0;
  for(const auto &line:diagnostics){
    unsigned stamp=0,attempt=0;
    assert(line.size()<240);
    assert(std::sscanf(line.c_str(),"GAMEBOY t_ms=%u load=%u",&stamp,&attempt)==2);
    assert(stamp>=last_time && attempt>=last_attempt);
    last_time=stamp;last_attempt=attempt;
    if(line.find("result=ok")!=std::string::npos && line.find("code=")!=std::string::npos)
      assert(line.find("code=0")!=std::string::npos);
  }
  auto find_log=[&](const char *value,size_t after=0){
    for(size_t i=after;i<diagnostics.size();++i)if(diagnostics[i].find(value)!=std::string::npos)return i;
    std::fprintf(stderr,"missing log %s in %s\n",value,scenario.c_str());std::abort();return size_t(0);
  };
  auto count_log=[&](const char *value){size_t n=0;for(const auto &line:diagnostics)if(line.find(value)!=std::string::npos)++n;return n;};
  if(scenario=="journey" || scenario=="file-handoff" || scenario=="rom-retry" || rom_failure_case()){
    const size_t scan=find_log("load=0 stage=directory-scan result=ok");
    size_t cursor=find_log(scenario=="rom-retry"?"load=2 stage=selected-rom-request result=begin":"load=1 stage=selected-rom-request result=begin",scan+1);
    if(scenario=="rom-retry"){assert(find_log("load=1 stage=selected-rom-load result=failed")<cursor);assert(count_log("stage=selected-rom-request result=begin")==2);assert(!count_log("load=1 stage=rom-first-playable-completion"));}
    cursor=find_log("stage=previous-cartridge-save result=ok",cursor+1);
    cursor=find_log("stage=rom-file-open result=begin",cursor+1);
    if(rom_failure_case()){
      const char *failure=scenario=="rom-open-failure"?"stage=rom-file-open result=failed":
          scenario=="rom-read-failure"?"stage=rom-read result=failed":"stage=rom-header-emulator-init result=failed";
      cursor=find_log(failure,cursor+1);
      cursor=find_log("stage=selected-rom-load result=failed",cursor+1);
      assert(diagnostics[cursor].find("code=-1")!=std::string::npos);
      assert(!count_log("stage=selected-rom-emulator result=start") && !count_log("stage=rom-first-playable-completion"));
    }else{
      for(const char *value:{"stage=rom-file-open result=ok","stage=rom-allocation result=ok","stage=rom-read result=ok", "stage=rom-header-emulator-init result=ok","stage=save-ram-restore result=ok","stage=save-state-restore result=skipped","stage=selected-rom-load result=ok","stage=selected-rom-emulator result=start","stage=rom-first-emulated-frame result=ok","stage=rom-first-playable-frame result=submit","stage=rom-first-playable-completion result=ok"})cursor=find_log(value,cursor+1);
      assert(count_log("stage=rom-first-playable-completion")==1);
      const auto &read=diagnostics[find_log("stage=rom-read result=ok")];
      assert(read.find("bytes=32768 read_calls=8")!=std::string::npos);
      assert(diagnostics[cursor].find("elapsed_ms=")!=std::string::npos);
    }
    cursor=find_log("stage=capability-release result=begin",cursor+1);
    find_log("stage=home-request result=begin",cursor+1);
    assert(count_log("stage=capability-release result=ok")==8);
  }
  std::printf("PASS %s frames=%u game=%u library=%u settings=%u saved=%u loaded=%u retained=%u calls=%u allocations=%zu\n",scenario.c_str(),frame_count,game_frames,library_frames,settings_frames,saved,loaded,retains,calls,app_memory.size());
  for(void *p:app_memory)std::free(p);
}
