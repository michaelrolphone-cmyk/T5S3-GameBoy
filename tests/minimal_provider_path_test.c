/* Link the production X4 transport/FatFs fixture to GameBoy's path boundary.
 * Only GPIO/clock/card wire are fake; provider paths and file bytes are real. */
#define main x4_provider_fixture_main
#include X4_SD_TEST_SOURCE
#undef main
#include "model.h"
int main(void){
 format(false);assert(START());assert(ready(NULL));
 const risc_storage_volume_api_v1* v=(const risc_storage_volume_api_v1*)&api;
 assert(!v->dir_open(v->context,"/sd"));
 uint32_t root=v->dir_open(v->context,gb_volume_path("/sd"));assert(root);
 v->dir_close(v->context,root);
 uint8_t bytes[4096];for(unsigned i=0;i<sizeof(bytes);++i)bytes[i]=(uint8_t)(i*17u+3u);
 uint32_t writer=v->file_open_write(v->context,"/demo.gb");assert(writer);
 for(unsigned n=0;n<8;++n)assert(v->file_write(v->context,writer,bytes,sizeof(bytes))==sizeof(bytes));
 assert(v->file_close(v->context,writer,true));
 uint64_t size=0;assert(!v->file_open_read(v->context,"/sd/demo.gb",&size));
 uint32_t reader=v->file_open_read(v->context,gb_volume_path("/sd/demo.gb"),&size);assert(reader && size==32768);
 uint8_t received[4096];
 for(unsigned n=0;n<8;++n){assert(v->file_read(v->context,reader,received,sizeof(received))==sizeof(received));assert(!memcmp(bytes,received,sizeof(bytes)));}
 assert(v->file_close(v->context,reader,false));
 verify_cleanup();free(card_image);
 puts("GameBoy + production X4 SD/FatFs: broker-prefix rejection, translated root/ROM reads and cleanup PASS");return 0;
}
