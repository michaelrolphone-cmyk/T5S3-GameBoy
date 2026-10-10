#include <assert.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <stdint.h>
int cap_test_vsnprintf(char *,size_t,const char *,va_list);
int cap_test_toupper(int);
char *cap_test_strtok_r(char *,const char *,char **);
static unsigned checks;
static void check(size_t capacity,const char *format,...){
 char expected[256],actual[256];memset(expected,0xa7,sizeof(expected));memset(actual,0xa7,sizeof(actual));
 va_list a,b;va_start(a,format);va_copy(b,a);
 int want=vsnprintf(expected,capacity,format,a),got=cap_test_vsnprintf(actual,capacity,format,b);
 va_end(a);va_end(b);assert(want==got);assert(!memcmp(expected,actual,sizeof(actual)));++checks;
}
int main(void){
 for(size_t n=0;n<128;++n){
  check(n,"%s %u %02u %.5s %%","entry",9u,4u,"message text");
  check(n,"%d %i %ld %lld",INT_MIN,-1,LONG_MIN,LLONG_MIN);
  check(n,"%u %lu %llu %zu",UINT_MAX,ULONG_MAX,ULLONG_MAX,(size_t)SIZE_MAX);
  check(n,"%08x %08X %-8s %05d",0x1234u,0xabcdefu,"OK",-31);
  check(n,"GAMEBOY %lu %.100s",12345ul,"file operation failed");
  check(n,"%.*s %c",3,"abcdef",'Q');
 }
 for(int c=-1;c<256;++c){assert(cap_test_toupper(c)==toupper(c));++checks;}
 const char *sets[]={"\n",",; ","",":/"};
 for(unsigned i=0;i<4;++i){char a[]="::a,,b; c\n\n/d/e",b[sizeof(a)];memcpy(b,a,sizeof(a));char *sa=NULL,*sb=NULL;
  char *x=strtok_r(a,sets[i],&sa),*y=cap_test_strtok_r(b,sets[i],&sb);
  while(x||y){assert(x&&y&&!strcmp(x,y));++checks;x=strtok_r(NULL,sets[i],&sa);y=cap_test_strtok_r(NULL,sets[i],&sb);}
  assert(!memcmp(a,b,sizeof(a)));
 }
 printf("PASS %u original-app libc compatibility checks\n",checks);
}
