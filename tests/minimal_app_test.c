#include <RiscRuntimeV1.h>
#include <RiscStorageVolumeV1.h>
#include <T5FileOpenApi.h>
#include "model.h"
#include "gbemu.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern int app_module_init(void);
extern void app_main(void);
extern void app_module_fini(void);
static const char* mode;
static bool core_ready,named_retention;
static unsigned diagnostics;
static uint32_t ticks,polls,submits,completes,reads,closed,dir_closed,releases,homes,game_frames;
static bool retained,live[4],acquired,file_live,dir_live;
static size_t offset;
static uint32_t present_until;
static uint8_t pixels[48000+32],rom[32768];
static bool is(const char* s){return !strcmp(mode,s);}
void* __real_malloc(size_t);
void* __real_calloc(size_t,size_t);
void* __wrap_malloc(size_t n){if((is("rom-oom") && n==32768) || (is("init-oom") && n==GBEMU_FRAMEBUFFER_SIZE))return NULL;return __real_malloc(n);}
void* __wrap_calloc(size_t n,size_t bytes){if(is("core-oom"))return NULL;return __real_calloc(n,bytes);}
static bool health(risc_runtime_health_v1* h){if(retained)return false;h->uptime_ms=ticks;return true;}
static void yield(uint32_t ms){assert(!retained && ms>=1 && ms<=50);ticks+=ms;assert(ticks<25000);}
static bool diagnostic(const char* s){
 assert(s && !retained && strlen(s)<256 && !strchr(s,'\n') && strstr(s,"GAMEBOY t_ms="));++diagnostics;assert(diagnostics<100);
 if(strstr(s,"stage=rom-core-init result=ok"))core_ready=true;
 if(strstr(s,"stage=retain-invocation result=required") && strlen(s)>70)named_retention=true;
 if(getenv("GAMEBOY_TEST_TRACE"))puts(s);
 return true;
}
static bool hold(void){retained=true;return true;}
static bool request_home(void){assert(!retained && !file_live && !acquired);for(unsigned i=0;i<4;++i)assert(!live[i]);++homes;return true;}
static bool info(void* c,risc_display_info_v1* out){(void)c;out->api_version=1;out->struct_size=sizeof(*out);out->width=is("landscape")?800:480;out->height=is("landscape")?480:800;out->supported_formats=is("bad-display")?0:1;return true;}
static bool acquire_frame(void* c,uint32_t format,risc_display_surface_v1* out){
 (void)c;assert(!retained && !acquired && format==1);assert(!present_until || ticks>=present_until);acquired=true;
 memset(pixels,0xa5,sizeof(pixels));*out=(risc_display_surface_v1){1,pixels+16,is("landscape")?800:480,is("landscape")?480:800,is("landscape")?100:60,is("surface-overflow")?1:48000,1};return true;
}
static void release_frame(void* c,risc_display_frame_v1 frame){(void)c;assert(!retained && acquired && frame==1);acquired=false;}
static bool submit(void* c,risc_display_frame_v1 frame,const risc_display_rect_v1* damage,size_t n,const risc_display_present_options_v1* options,risc_display_present_token_v1* token){
 (void)c;(void)damage;assert(!retained && acquired && frame==1 && n==0 && options);acquired=false;
 for(unsigned i=0;i<16;++i)assert(pixels[i]==0xa5 && pixels[48016+i]==0xa5);
 if(!submits)assert(options->intent==RISC_DISPLAY_PRESENT_LOW_LATENCY);
 if(core_ready){
  ++game_frames;unsigned black=0;for(unsigned i=16;i<48016;++i)black+=(unsigned)__builtin_popcount(pixels[i]);
  if(game_frames>1)assert(black>1000); // The real synthetic ROM drew tile pixels.
  const char* capture=getenv("GAMEBOY_TEST_FRAME");
  if(capture && is("chooser") && game_frames==2){FILE* f=fopen(capture,"wb");assert(f);assert(fprintf(f,"P4\n480 800\n")>0);assert(fwrite(pixels+16,1,48000,f)==48000);assert(!fclose(f));}
 }
 ++submits;*token=submits;present_until=ticks+40;return true;
}
static bool status(void* c,risc_display_present_token_v1 token,risc_display_present_status_v1* out){
 (void)c;assert(!retained && token==submits);out->state=is("present-failed")?RISC_DISPLAY_PRESENT_FAILED:is("present-timeout")?RISC_DISPLAY_PRESENT_ACTIVE:ticks<present_until?RISC_DISPLAY_PRESENT_ACTIVE:RISC_DISPLAY_PRESENT_COMPLETE;
 if(out->state==RISC_DISPLAY_PRESENT_COMPLETE){++completes;present_until=0;}return true;
}
static bool navigation_poll(void* c,risc_input_navigation_frame_v1* out){
 (void)c;assert(!retained);++polls;memset(out,0,sizeof(*out));
 if(is("present-timeout") || is("present-failed") || is("surface-overflow"))return true;
 if(is("navigation-failure-pending") && submits && present_until>ticks)return false;
 if(is("cancel") && file_live){out->pressed=RISC_NAV_BACK;return true;}
 if(submits>=4 || (polls>1000 && !file_live)){out->pressed=RISC_NAV_HOME;return true;}
 if(is("paper") && polls==2){out->pressed=RISC_NAV_PAGE_BACK;return true;}
 if(!file_live && polls==(is("paper")?3u:2u) && !is("receiver") && !is("invalid-source")){out->pressed=RISC_NAV_CONFIRM;out->buttons=RISC_NAV_CONFIRM;}
 return true;
}
static bool reset(void* c){(void)c;assert(!retained);return true;}
static bool refresh(void* c){(void)c;return !is("unavailable-media");}
static bool ready(void* c){(void)c;return !is("unavailable-media");}
static risc_storage_dir_t dir_open(void* c,const char* p){(void)c;assert(!retained && !dir_live && !strcmp(p,"/"));dir_live=true;return 1;}
static bool dir_next(void* c,risc_storage_dir_t d,risc_storage_dirent_v1* out){(void)c;assert(dir_live && d==1);static unsigned n;if(n++==(is("directory-limit")?600u:1u))return false;snprintf(out->name,sizeof(out->name),is("directory-limit")?"%03u.gb":"demo.gb",n);out->size=sizeof(rom);return true;}
static void dir_close(void* c,risc_storage_dir_t d){(void)c;assert(!retained && dir_live && d==1);dir_live=false;++dir_closed;}
static risc_storage_file_t open_read(void* c,const char* p,uint64_t* bytes){(void)c;assert(!retained && !strcmp(p,"/demo.gb") && !file_live);offset=0;file_live=true;*bytes=is("size-limit")?5*1024*1024:sizeof(rom);return 1;}
static size_t read_rom(void* c,risc_storage_file_t f,void* out,size_t n){(void)c;assert(!retained && file_live && f==1 && n<=4096);++reads;if(is("error-read"))return 0;if(is("short-read") && n>97)n=97;if(n>sizeof(rom)-offset)n=sizeof(rom)-offset;memcpy(out,rom+offset,n);offset+=n;return n;}
static bool close_rom(void* c,risc_storage_file_t f,bool commit){(void)c;assert(!retained && file_live && f==1 && !commit);++closed;if(is("close-retained"))return false;file_live=false;return true;}
static bool source(char* out,size_t cap){assert(cap==512);if(!is("receiver") && !is("invalid-source"))return false;strcpy(out,is("invalid-source")?"/sd/../bad.gb":"/sd/demo.gb");return true;}
static const risc_display_output_api_v1 display={1,sizeof(display),NULL,info,acquire_frame,release_frame,submit,status,NULL,NULL};
static const risc_input_navigation_api_v1 nav={1,sizeof(nav),NULL,navigation_poll,NULL,reset};
static const risc_storage_volume_api_v1 storage={1,sizeof(storage),NULL,refresh,ready,NULL,NULL,dir_open,dir_next,dir_close,open_read,read_rom,NULL,NULL,close_rom,NULL,NULL};
static const t5_file_open_api_v1 files={1,sizeof(files),NULL,NULL,NULL,NULL,NULL,source};
static bool acquire(const char* cap,uint32_t version,uint64_t instance,risc_runtime_capability_v1* out){
 const char* names[]={"display.output","input.navigation","storage.volume","file.open"};const void* apis[]={&display,&nav,&storage,&files};assert(version==1 && instance==0);
 for(unsigned i=0;i<4;++i)if(!strcmp(names[i],cap)){if(is("missing-grant") && i==2)return false;assert(!live[i]);live[i]=true;out->slot=i+1;out->generation=1;out->api=apis[i];return true;}return false;
}
static bool release(risc_runtime_capability_v1* g){assert(!retained && g->slot && g->slot<=4 && live[g->slot-1]);if(is("grant-retained"))return false;live[g->slot-1]=false;memset(g,0,sizeof(*g));++releases;return true;}
static risc_runtime_api_v1 runtime={.api_version=1,.struct_size=sizeof(runtime),.health=health,.yield_ms=yield,.diagnostic=diagnostic,.acquire=acquire,.release=release,.retain_invocation=hold,.request_default=request_home};
const risc_runtime_api_v1* risc_runtime_get_api(uint32_t v){return v==1?&runtime:NULL;}
int main(int argc,char** argv){
 assert(argc==2);mode=argv[1];rom[0x100]=0xc3;rom[0x101]=0x50;rom[0x102]=1;memcpy(rom+0x134,"TEST ROM",8);
 /* Original synthetic DMG program: odd-address instruction operands and
  * word RAM reads/writes, a black tile row, then a stable LCD loop. */
 const uint8_t program[]={0x31,0x01,0xc1,0x08,0x03,0xc0,0x31,0x03,0xc0,0xc1,0x21,0x00,0x80,0x36,0xff,0x23,0x36,0xff,0x3e,0x91,0xe0,0x40,0x18,0xfe};
 memcpy(rom+0x150,program,sizeof(program));
 if(is("cgb-only"))rom[0x143]=0xc0;
 for(unsigned i=0x134;i<=0x14c;++i)rom[0x14d]=(uint8_t)(rom[0x14d]-rom[i]-1);
 if(is("bad-rom"))rom[0x147]=0xff; // Unsupported mapper, not an invented checksum policy.
 if(is("old-runtime"))runtime.struct_size=RISC_RUNTIME_STREAM_CLIENT_V1_SIZE;
 int init=app_module_init();bool reject=is("old-runtime") || is("bad-display") || is("missing-grant") || is("init-oom");assert((init!=0)==reject);
 if(!init){app_main();app_module_fini();}
 bool retain=is("close-retained") || is("present-timeout") || is("present-failed") || is("grant-retained");assert(retained==retain);
 if(!retain){for(unsigned i=0;i<4;++i)assert(!live[i]);assert(!file_live && !acquired);}
 assert(homes==(!reject && !retain && !is("surface-overflow") && !is("navigation-failure-pending")));
 fprintf(stderr,"observed %s: reads=%u closed=%u submits=%u complete=%u polls=%u ticks=%u\n",mode,reads,closed,submits,completes,polls,ticks);
 if(is("chooser") || is("receiver") || is("landscape")){assert(reads==8 && closed==1 && submits>=4 && completes==submits);}
 if(is("chooser") || is("receiver") || is("landscape") || is("paper"))assert(game_frames>=2);if(is("bad-rom") || is("cgb-only") || is("init-oom") || is("rom-oom") || is("core-oom"))assert(game_frames==0);
 if(is("rom-oom"))assert(closed==1 && reads==0);if(is("core-oom"))assert(closed==1 && reads==8);
 if(is("short-read"))assert(reads>8 && closed==1);if(is("cancel"))assert(reads==1 && closed==1);if(is("size-limit"))assert(reads==0 && closed==1);if(is("invalid-source"))assert(reads==0);
 if(retain)assert(named_retention);if(!is("old-runtime"))assert(diagnostics);
 printf("Minimal GameBoy real core: %s PASS (%u reads, %u frames)\n",mode,reads,submits);
}
