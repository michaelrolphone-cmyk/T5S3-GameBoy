#include "model.h"
#include "gbemu.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern uint64_t __udivdi3(uint64_t,uint64_t);
extern uint64_t __umoddi3(uint64_t,uint64_t);
int main(void){
 const char* invalid[]={"", "/", "/sd/", "/sd/../a.gb", "/sd//a.gb", "/sd/./a.gb", "/sd/a\\b.gb", "/sd/a\n.gb", "/sdx/a.gb"};
 for(unsigned i=0;i<sizeof(invalid)/sizeof(*invalid);++i)assert(!gb_path(invalid[i]));
 assert(!strcmp(gb_volume_path("/sd"),"/") && !strcmp(gb_volume_path("/sd/ROMs/demo.gb"),"/ROMs/demo.gb") && !gb_volume_path("/sd/../demo.gb") && !gb_volume_path("/sdx/demo.gb"));
 assert(gb_path("/sd") && gb_path("/sd/ROMs/Test.GB"));assert(gb_rom_name("Test.GB") && gb_rom_name("dual.GbC"));
 assert(!gb_rom_name(".gb") && !gb_rom_name("a.txt") && !gb_rom_name("../a.gb"));
 char joined[512];assert(gb_join("/sd","a.gb",joined,sizeof(joined)) && !strcmp(joined,"/sd/a.gb"));
 assert(!gb_join("/sd","../a.gb",joined,sizeof(joined)) && !*joined);
 strcpy(joined,"/sd/ROMs/a.gb");gb_parent(joined);assert(!strcmp(joined,"/sd/ROMs"));gb_parent(joined);gb_parent(joined);assert(!strcmp(joined,"/sd"));
 gb_catalog c={0};assert(gb_catalog_add(&c,"z.gb",false));assert(gb_catalog_add(&c,"a",true));assert(gb_catalog_add(&c,"b.gb",false));assert(c.count==3 && c.entries[0].directory && !strcmp(c.entries[1].name,"b.gb"));
 for(unsigned n=0;n<100;++n){char name[32];snprintf(name,sizeof(name),"%u.gb",n);(void)gb_catalog_add(&c,name,false);}assert(c.count==96 && c.truncated);
 assert(gb_buttons(RISC_NAV_HOME)==0 && gb_buttons(0xff)==255);
 for(unsigned rotation=0;rotation<2;++rotation){
  unsigned w=rotation?480:800,h=rotation?800:480,stride=(w+7)/8;
  uint8_t* memory=malloc((size_t)stride*h+32);memset(memory,0xa5,(size_t)stride*h+32);
  risc_display_surface_v1 s={1,memory+16,w,h,stride,stride*h,RISC_DISPLAY_FORMAT_MONO1};unsigned lw,lh;assert(gb_surface(&s,&lw,&lh) && lw==800 && lh==480);
  memset(s.pixels,0,s.size_bytes);uint8_t mono[GBEMU_FRAMEBUFFER_SIZE];memset(mono,0xff,sizeof(mono));assert(gb_blit(&s,mono,sizeof(mono)));
  for(unsigned i=0;i<s.size_bytes;++i)assert(((uint8_t*)s.pixels)[i]==0); // Core white=1, canonical MONO1 black=1.
  memset(mono,0,sizeof(mono));assert(gb_blit(&s,mono,sizeof(mono)));unsigned black=0;for(unsigned i=0;i<s.size_bytes;++i)black+=(unsigned)__builtin_popcount(((uint8_t*)s.pixels)[i]);assert(black==480*432);
  gb_text(&s,-5,-5,"test",3);gb_pixel(&s,2000,2000,true);
  for(unsigned i=0;i<16;++i)assert(memory[i]==0xa5 && memory[16+s.size_bytes+i]==0xa5);
  --s.size_bytes;assert(!gb_surface(&s,&lw,&lh));free(memory);
 }
 uint64_t seed=7;
 for(unsigned i=0;i<10000;++i){seed=seed*UINT64_C(6364136223846793005)+1;uint64_t n=seed;seed=seed*UINT64_C(6364136223846793005)+1;uint64_t d=seed|1;assert(__udivdi3(n,d)==n/d && __umoddi3(n,d)==n%d);}
 const uint64_t values[]={0,1,2,UINT32_MAX,UINT64_C(1)<<63,UINT64_MAX};
 for(unsigned a=0;a<6;++a)for(unsigned b=1;b<6;++b)assert(__udivdi3(values[a],values[b])==values[a]/values[b] && __umoddi3(values[a],values[b])==values[a]%values[b]);
 puts("Minimal model: bounded SD paths/catalog, navigation, both MONO1 orientations and 10030 integer divisions PASS");
}
