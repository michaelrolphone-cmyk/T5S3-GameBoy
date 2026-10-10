#pragma once
#include "platform.hpp"
#include <stdio.h>
// Operation-only Runtime ring logging: no storage drain or frame telemetry.
struct CapLoadTrace {
 const char *stage;uint32_t began;bool ended;
 explicit CapLoadTrace(const char *name,const char *detail=nullptr):stage(name),began(cap_ready()?cap_millis():0),ended(false){if(cap_ready())cap_log(stage,"begin",detail);}
 void end(bool ok,int code=0,const char *detail=nullptr){
  if(ended)return;
  ended=true;
  if(!cap_ready())return;
  if(!ok && code==0)code=-1;
  char value[128];snprintf(value,sizeof(value),"elapsed_ms=%lu code=%d %.68s",(unsigned long)(cap_millis()-began),code,detail?detail:"");
  cap_log(stage,ok?"ok":"failed",value);
 }
 ~CapLoadTrace(){if(!ended)end(false);}
};
