/* App-local compatibility for the original UI's three imports absent from the
 * small native symbol table. Integer/string diagnostics only; no floating point. */
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <limits.h>
int toupper(int c){return c>='a' && c<='z'?c-'a'+'A':c;}
static int member(char c,const char *set){while(*set)if(c==*set++)return 1;return 0;}
char *strtok_r(char *text,const char *delimiters,char **save){
 char *s=text?text:*save;if(!s)return NULL;
 while(*s && member(*s,delimiters))++s;
 if(!*s){*save=s;return NULL;}char *start=s;
 while(*s && !member(*s,delimiters))++s;
 if(*s)*s++=0;*save=s;return start;
}
typedef struct{char *out;size_t cap,used;} writer;
static void put(writer*w,char c){if(w->out && w->used+1<w->cap)w->out[w->used]=c;++w->used;}
int vsnprintf(char *out,size_t capacity,const char *format,va_list args){
 writer w={out,capacity,0};
 while(*format){
  if(*format!='%'){put(&w,*format++);continue;}++format;
  if(*format=='%'){put(&w,*format++);continue;}
  int left=0,zero=0;while(*format=='-' || *format=='0'){if(*format=='-')left=1;else zero=1;++format;}
  unsigned width=0;while(*format>='0'&&*format<='9'){if(width<1024)width=width*10+(unsigned)(*format-'0');++format;}
  int precision=-1;
  if(*format=='.'){++format;precision=0;if(*format=='*'){precision=va_arg(args,int);++format;}else while(*format>='0'&&*format<='9'){if(precision<1024)precision=precision*10+*format-'0';++format;}}
  unsigned length=0;if(*format=='z'){length=3;++format;}else while(*format=='l' && length<2){++length;++format;}
  char kind=*format;if(!kind)break;++format;
  char number[32];const char *text=number;size_t count=0;int negative=0;
  if(kind=='s'){
   text=va_arg(args,const char*);if(!text)text="(null)";
   while(text[count] && (precision<0 || count<(size_t)precision))++count;zero=0;
  }else if(kind=='c'){number[0]=(char)va_arg(args,int);count=1;zero=0;}
  else if(kind=='d'||kind=='i'||kind=='u'||kind=='x'||kind=='X'){
   uint64_t value;
   if(kind=='d'||kind=='i'){
    int64_t signed_value=length==2?va_arg(args,long long):length==1?va_arg(args,long):length==3?(int64_t)va_arg(args,ptrdiff_t):va_arg(args,int);
    negative=signed_value<0;value=negative?(uint64_t)(-(signed_value+1))+1:(uint64_t)signed_value;
   }else value=length==2?va_arg(args,unsigned long long):length==1?va_arg(args,unsigned long):length==3?va_arg(args,size_t):va_arg(args,unsigned);
   unsigned radix=kind=='x'||kind=='X'?16:10;char reversed[32];size_t n=0;
   do{unsigned digit=(unsigned)(value%radix);reversed[n++]=(char)(digit<10?'0'+digit:(kind=='X'?'A':'a')+digit-10);value/=radix;}while(value);
   if(negative)number[count++]='-';while(n)number[count++]=reversed[--n];
  }else{put(&w,'%');put(&w,kind);continue;}
  if(width>2048)width=2048;
  unsigned padding=width>count?width-(unsigned)count:0;
  if(!left){if(zero && negative){put(&w,'-');++text;--count;}for(unsigned i=0;i<padding;++i)put(&w,zero?'0':' ');}
  for(size_t i=0;i<count;++i)put(&w,text[i]);
  if(left)for(unsigned i=0;i<padding;++i)put(&w,' ');
 }
 if(out && capacity)out[w.used<capacity?w.used:capacity-1]=0;
 return w.used>INT_MAX?INT_MAX:(int)w.used;
}
