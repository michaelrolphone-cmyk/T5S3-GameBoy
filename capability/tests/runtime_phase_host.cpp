#include "bootstrap/Runtime.h"
extern "C" int32_t test_native_time_read(void*,risc_realtime_snapshot_v1*);
extern "C" bool test_native_health(risc_runtime_health_v1*);
extern "C" void test_native_delay(uint32_t);
static bool owner(){return true;}
static bool log(const char*){return true;}
static int32_t seed(void*,int64_t,uint32_t){return RISC_REALTIME_INVALID;}
static bool bind(RiscBoot::Runtime& r){
  static risc_realtime_control_api_v1 table{1,sizeof(table),nullptr,test_native_time_read,seed};
  return r.registerRealtime(&table);
}
bool test_run_native_phase(const char* root){
  RiscBoot::Runtime actual({owner,test_native_health,test_native_delay,log,bind});
  return actual.prepare(root) && actual.run();
}
