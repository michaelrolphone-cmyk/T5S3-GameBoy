/* Actual selected SD transport/FatFs. Only card wire and native hardware are fixtures. */
#define main x4_provider_fixture_main
#include X4_SD_TEST_SOURCE
#undef main
#define BASE (*(const risc_storage_volume_api_v1*)&api)
static risc_storage_volume_api_v1_ext observed_api;
static uint64_t file_reads,file_opens,stat_calls,directory_reads,read_bytes;
static size_t observed_read(void*c,uint32_t h,void*b,size_t n){++file_reads;assert(n<=4096);const size_t got=BASE.file_read(c,h,b,n);read_bytes+=got;return got;}
static uint32_t observed_open(void*c,const char*p,uint64_t*n){++file_opens;return BASE.file_open_read(c,p,n);}
static bool observed_stat(void*c,const char*p,uint64_t*n,bool*d){++stat_calls;return BASE.stat(c,p,n,d);}
static bool observed_next(void*c,uint32_t h,risc_storage_dirent_v1*d){++directory_reads;return BASE.dir_next(c,h,d);}
const risc_storage_volume_api_v1* load_fixture_start(size_t size){
 format(false);assert(START());assert(ready(NULL));
 const uint32_t writer=BASE.file_open_write(NULL,"/Test.gb");assert(writer);
 uint8_t bytes[4096];for(unsigned i=0;i<sizeof(bytes);++i)bytes[i]=(uint8_t)(i*17u+3u);
 for(size_t n=0;n<size;n+=sizeof(bytes))assert(BASE.file_write(NULL,writer,bytes,sizeof(bytes))==sizeof(bytes));
 assert(BASE.file_close(NULL,writer,true));
 memcpy(&observed_api,&api,sizeof(observed_api));observed_api.base.struct_size=sizeof(observed_api);observed_api.base.file_read=observed_read;observed_api.base.file_open_read=observed_open;
 observed_api.base.stat=observed_stat;observed_api.base.dir_next=observed_next;
 return &observed_api.base;
}
void load_fixture_reset_counts(void){file_reads=file_opens=stat_calls=directory_reads=read_bytes=0;card_reads=card_writes=0;sleeps=0;calls=0;}
void load_fixture_report(const char*phase,size_t bytes){
 if(!strcmp(phase,"selected-rom")){assert(read_bytes==bytes);assert(file_reads==(bytes+4095)/4096);assert(file_opens==2);assert(!stat_calls && !directory_reads && !card_writes);}
 if(!strcmp(phase,"initial-scan")){assert(!read_bytes && !file_reads && !card_writes);}
 printf("PROVIDER phase=%s payload_bytes=%zu file_opens=%llu file_reads=%llu read_bytes=%llu stat_calls=%llu directory_reads=%llu sector_reads=%u sector_writes=%u gpio_calls=%u sleeps=%u\n",
 phase,bytes,(unsigned long long)file_opens,(unsigned long long)file_reads,(unsigned long long)read_bytes,(unsigned long long)stat_calls,(unsigned long long)directory_reads,card_reads,card_writes,calls,sleeps);
}
uint32_t load_fixture_millis(void){return (uint32_t)now_ms;}
void load_fixture_yield(void){++now_ms;}
void load_fixture_end(void){verify_cleanup();free(card_image);}

void load_fixture_stat_contract(void){
 uint64_t size=0;bool directory=true;
 assert(BASE.stat(NULL,"/Test.gb",&size,&directory) && size>=32768 && !directory);
 assert(!BASE.stat(NULL,"/Test.gb",NULL,&directory));
 assert(!BASE.stat(NULL,"/Test.gb",&size,NULL));
 assert(!BASE.stat(NULL,"/Test.gb",NULL,NULL));
 const char config[]="last_rom=/sd/Test.gb\naudio_engine=0\n";
 const uint32_t h=BASE.file_open_write(NULL,"/paperboy.cfg");assert(h);
 assert(BASE.file_write(NULL,h,config,sizeof(config)-1)==sizeof(config)-1);
 assert(BASE.file_close(NULL,h,true));
 puts("CONTRACT real FatFs stat requires both outputs; null outputs refuse an existing ROM");
}
