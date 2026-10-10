#include "platform.hpp"
#include "paperboy_storage.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <cstring>
#include <vector>
extern "C" {
const risc_storage_volume_api_v1* load_fixture_start(size_t);
void load_fixture_stat_contract();
void load_fixture_reset_counts();void load_fixture_report(const char*,size_t);
uint32_t load_fixture_millis();void load_fixture_yield();void load_fixture_end();
}
static const risc_storage_volume_api_v1* volume;
bool cap_ready(){return true;}bool cap_retained(){return false;}bool cap_running(){return true;}
void cap_hold(const char*stage){fprintf(stderr,"unexpected retained stage=%s\n",stage);abort();}
void cap_log(const char*s,const char*r,const char*d){printf("LOAD stage=%s result=%s %s\n",s,r,d?d:"");}
uint32_t cap_millis(){return load_fixture_millis();}void cap_yield(){load_fixture_yield();}
void* cap_alloc(size_t n){return malloc(n);}void cap_free(void*p){free(p);}
const risc_storage_volume_api_v1* cap_storage(){return volume;}
int main(int argc,char**argv){
 assert(argc==2 || argc==3);const size_t size=strtoul(argv[1],nullptr,10);assert(size>=32768&&size<=4194304&&size%4096==0);
 volume=load_fixture_start(size);load_fixture_reset_counts();
 assert(paperboy_storage_begin());load_fixture_report("initial-scan",0);
 assert(paperboy_storage_status().rom_count==1);
 if(argc==3){
   load_fixture_stat_contract();
   assert(paperboy_storage_file_exists("/sd/Test.gb"));
   PaperboyStorageConfig config{};assert(paperboy_storage_read_config(config));
   assert(std::strcmp(config.last_rom,"/sd/Test.gb")==0);
   config.audio_engine=1;assert(paperboy_storage_write_config(config));
   PaperboyStorageConfig restored{};assert(paperboy_storage_read_config(restored));
   assert(restored.audio_engine==1 && std::strcmp(restored.last_rom,"/sd/Test.gb")==0);
   char state_path[PAPERBOY_STORAGE_PATH_MAX];
   assert(paperboy_storage_make_state_path("/sd/Test.gb",state_path,sizeof(state_path)));
   std::vector<unsigned char> state(51340),readback(state.size());
   for(size_t i=0;i<state.size();++i)state[i]=(unsigned char)(i*31+7);
   assert(paperboy_storage_write_blob_atomic(state_path,state.data(),state.size()));
   assert(paperboy_storage_file_exists(state_path));size_t got=0;
   assert(paperboy_storage_read_blob(state_path,readback.data(),readback.size(),got));
   assert(got==state.size() && readback==state);
   state[0]^=0xff;assert(paperboy_storage_write_blob_atomic(state_path,state.data(),state.size()));
   assert(paperboy_storage_read_blob(state_path,readback.data(),readback.size(),got));
   assert(got==state.size() && readback==state);
   load_fixture_report("saved-state",state.size());
   puts("PASS actual SD/FatFs existing ROM/config detection and exact 51340-byte saved-state overwrite/readback");
   paperboy_storage_end();load_fixture_end();return 0;
 }
 load_fixture_reset_counts();
 PaperboyRomData rom{};const auto begin=std::chrono::steady_clock::now();
 assert(paperboy_storage_load_rom("/sd/Test.gb",rom));
 const double elapsed=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();
 assert(rom.size==size);for(size_t i=0;i<size;++i)assert(rom.data[i]==(uint8_t)(i*17u+3u));
 load_fixture_report("selected-rom",size);printf("HOST actual SD/FatFs wire-model elapsed_ms=%.3f (not hardware timing)\n",elapsed);
 paperboy_storage_free_rom(rom);paperboy_storage_end();load_fixture_end();
}
