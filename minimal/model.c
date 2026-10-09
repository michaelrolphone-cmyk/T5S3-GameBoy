#include "model.h"
#include "font8x8_basic.h"
#include "gbemu.h"
#include <string.h>
static size_t bounded_length(const char* s,size_t max){size_t n=0;while(n<max && s[n])++n;return n;}
static bool component(const char* s,size_t n){
 if(!n || n>=128 || (n==1 && s[0]=='.') || (n==2 && s[0]=='.' && s[1]=='.'))return false;
 for(size_t i=0;i<n;++i)if((unsigned char)s[i]<32 || s[i]==127 || s[i]=='/' || s[i]=='\\')return false;
 return true;
}
bool gb_path(const char* p){
 if(!p || bounded_length(p,GB_PATH_MAX)>=GB_PATH_MAX || strncmp(p,"/sd",3))return false;
 if(!p[3])return true;
 if(p[3]!='/')return false;
 const char* s=p+4;
 for(const char* q=s;;++q)if(!*q || *q=='/'){if(!component(s,(size_t)(q-s)))return false;if(!*q)return true;s=q+1;}
}
const char* gb_volume_path(const char* path){
 if(!gb_path(path))return NULL;
 return path[3]?path+3:"/";
}
bool gb_rom_name(const char* name){
 if(!name || !component(name,bounded_length(name,128)))return false;
 const char* ext=strrchr(name,'.');if(!ext || ext==name)return false;
 char lower[5]={0};size_t n=strlen(ext);if(n>4)return false;
 for(size_t i=0;i<n;++i)lower[i]=ext[i]>='A' && ext[i]<='Z'?(char)(ext[i]+32):ext[i];
 return !strcmp(lower,".gb") || !strcmp(lower,".gbc");
}
bool gb_join(const char* dir,const char* name,char* out,size_t cap){
 if(!out || !cap)return false;out[0]=0;
 if(!gb_path(dir) || !name || !component(name,bounded_length(name,128)))return false;
 size_t a=strlen(dir),b=strlen(name);if(a+b+2>cap || a+b+2>GB_PATH_MAX)return false;
 memcpy(out,dir,a);out[a]='/';memcpy(out+a+1,name,b+1);return true;
}
void gb_parent(char* path){if(!path || !gb_path(path))return;char* slash=strrchr(path,'/');if(slash && slash>path+2)*slash=0;}
bool gb_catalog_add(gb_catalog* c,const char* name,bool dir){
 if(!c || !name || !component(name,bounded_length(name,128)) || (!dir && !gb_rom_name(name)))return false;
 if(c->count==GB_CATALOG_MAX){c->truncated=true;return false;}
 size_t at=c->count;
 while(at && ((dir && !c->entries[at-1].directory) || (dir==c->entries[at-1].directory && strcmp(name,c->entries[at-1].name)<0))){c->entries[at]=c->entries[at-1];--at;}
 strcpy(c->entries[at].name,name);c->entries[at].directory=dir;++c->count;return true;
}
uint8_t gb_buttons(uint32_t n){
 return (n&RISC_NAV_CONFIRM?GBEMU_INPUT_A:0) | (n&RISC_NAV_BACK?GBEMU_INPUT_B:0) |
 (n&RISC_NAV_PAGE_BACK?GBEMU_INPUT_SELECT:0) | (n&RISC_NAV_PAGE_FORWARD?GBEMU_INPUT_START:0) |
 (n&RISC_NAV_LEFT?GBEMU_INPUT_LEFT:0) | (n&RISC_NAV_RIGHT?GBEMU_INPUT_RIGHT:0) |
 (n&RISC_NAV_UP?GBEMU_INPUT_UP:0) | (n&RISC_NAV_DOWN?GBEMU_INPUT_DOWN:0);
}
bool gb_surface(const risc_display_surface_v1* s,unsigned* w,unsigned* h){
 if(!s || !s->frame || !s->pixels || s->pixel_format!=RISC_DISPLAY_FORMAT_MONO1 ||
 s->width<144 || s->height<144 || s->width>1024 || s->height>1024 ||
 s->stride_bytes<(s->width+7)/8 || s->stride_bytes>256 ||
 s->size_bytes<(uint64_t)s->stride_bytes*s->height)return false;
 *w=s->width>s->height?s->width:s->height;*h=s->width>s->height?s->height:s->width;
 return *w>=480 && *h>=432;
}
void gb_pixel(risc_display_surface_v1* s,int x,int y,bool black){
 int w=(int)s->width,h=(int)s->height;
 if(h>w){if(x<0 || y<0 || x>=h || y>=w)return;int old=x;x=w-1-y;y=old;}
 else if(x<0 || y<0 || x>=w || y>=h)return;
 uint8_t* p=(uint8_t*)s->pixels+(size_t)y*s->stride_bytes+(unsigned)x/8;
 uint8_t mask=(uint8_t)(0x80u>>((unsigned)x&7));if(black)*p|=mask;else *p&=(uint8_t)~mask;
}
void gb_text(risc_display_surface_v1* s,int x,int y,const char* text,unsigned scale){
 if(!text || !scale || scale>3)return;
 for(unsigned n=0;text[n] && n<100;++n,x+=(int)(8*scale)){
  unsigned ch=(unsigned char)text[n];if(ch>=128)ch='?';
  for(unsigned row=0;row<8;++row)for(unsigned col=0;col<8;++col)if(font8x8_basic[ch][row]&(1u<<col))
   for(unsigned dy=0;dy<scale;++dy)for(unsigned dx=0;dx<scale;++dx)gb_pixel(s,x+(int)(col*scale+dx),y+(int)(row*scale+dy),true);
 }
}
bool gb_blit(risc_display_surface_v1* s,const uint8_t* source,size_t bytes){
 unsigned w,h;if(!source || bytes<GBEMU_FRAMEBUFFER_SIZE || !gb_surface(s,&w,&h))return false;
 unsigned scale=(h-32)/GBEMU_SOURCE_HEIGHT;if(scale>3)scale=3;if(!scale)return false;
 int ox=(int)(w-GBEMU_SOURCE_WIDTH*scale)/2,oy=(int)(h-GBEMU_SOURCE_HEIGHT*scale)/2+10;
 for(unsigned y=0;y<GBEMU_SOURCE_HEIGHT*scale;++y)for(unsigned x=0;x<GBEMU_SOURCE_WIDTH*scale;++x){
  unsigned sx=x/scale*GBEMU_SCALE,sy=y/scale*GBEMU_SCALE;
  bool white=(source[(size_t)sy*GBEMU_FRAME_PITCH_BYTES+sx/8]&(0x80u>>(sx&7)))!=0;
  gb_pixel(s,ox+(int)x,oy+(int)y,!white);
 }
 return true;
}
