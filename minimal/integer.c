/* Position-independent integer helpers for the selected Xtensa GCC8 port.
 * Its archive's division helpers are not linkable as this ET_DYN module.
 * Fixed 64-step restoring division, including the carry beyond bit63. */
#include <stdint.h>
static uint64_t divide(uint64_t n,uint64_t d,uint64_t* remainder){
 if(!d)__builtin_trap();
 uint64_t q=0,r=0;
 for(unsigned bit=64;bit--;){
  uint64_t carry=r>>63;r=(r<<1)|((n>>bit)&1u);
  if(carry || r>=d){r-=d;q|=UINT64_C(1)<<bit;}
 }
 *remainder=r;return q;
}
uint64_t __udivdi3(uint64_t n,uint64_t d){uint64_t r;return divide(n,d,&r);}
uint64_t __umoddi3(uint64_t n,uint64_t d){uint64_t r;(void)divide(n,d,&r);return r;}
int abs(int n){return n<0?(int)(0u-(unsigned)n):n;}
