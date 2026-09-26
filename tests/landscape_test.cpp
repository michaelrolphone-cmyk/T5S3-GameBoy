#include "paperboy_landscape.h"
#include <cassert>
#include <cstring>
#include <vector>
#include <cstdio>
static touch_state_t point(unsigned x,unsigned y) {
  touch_state_t t{}; t.touched=true; t.points=1;
  if (paperboy_orientation()==PaperboyOrientation::LandscapeReverse) { x=959-x; y=539-y; }
  t.x[0]=539-y; t.y[0]=x; return t;
}
static bool pixel(const std::vector<uint8_t>& p,int x,int y) { return p[y*120+x/8] & (0x80>>(x%8)); }
int main() {
  assert(!paperboy_is_landscape());
  for (int mode=0;mode<2;++mode) {
    paperboy_orientation_cycle(); assert(paperboy_is_landscape());
    for (auto xy : {std::pair<unsigned,unsigned>{0,0},{959,539},{240,54},{719,485}}) {
      auto t=point(xy.first,xy.second); uint16_t x,y;
      paperboy_landscape_touch(t.x[0],t.y[0],x,y); assert(x==xy.first && y==xy.second);
    }
    struct Case { unsigned x,y; uint8_t mask; };
    for (auto c : {Case{40,270,GBEMU_INPUT_LEFT},Case{190,270,GBEMU_INPUT_RIGHT},Case{116,190,GBEMU_INPUT_UP},Case{116,350,GBEMU_INPUT_DOWN},Case{864,230,GBEMU_INPUT_A},Case{788,326,GBEMU_INPUT_B},Case{80,454,GBEMU_INPUT_SELECT},Case{820,454,GBEMU_INPUT_START}}) {
      auto t=point(c.x,c.y); assert(paperboy_landscape_buttons(&t)==c.mask);
    }
    auto a=point(864,230),left=point(40,270); a.points=2;a.x[1]=left.x[0];a.y[1]=left.y[0];
    assert(paperboy_landscape_buttons(&a)==(GBEMU_INPUT_A|GBEMU_INPUT_LEFT));
    auto center=point(480,270); assert(paperboy_landscape_buttons(&center)==0 && paperboy_landscape_actions(&center)==0);
    auto rotate=point(820,28); assert(paperboy_landscape_actions(&rotate)==PAPERBOY_ACTION_ROTATE);
    auto full=point(620,512); assert(paperboy_landscape_actions(&full)==PAPERBOY_ACTION_FULLSCREEN);
    std::vector<uint8_t> game(GBEMU_FRAMEBUFFER_SIZE,255),canvas(65280+16,0xCC),panel(64800+16,0xCC);
    game[0]=0x7F; // one black game pixel at top left
    paperboy_landscape_draw(canvas.data(),panel.data(),game.data(),0,true,false,nullptr,nullptr,0);
    int x=240,y=54; if(mode) {x=959-x;y=539-y;}
    assert(pixel(panel,x,y));
    for(size_t i=64800;i<panel.size();++i) assert(panel[i]==0xCC);
    for(size_t i=64800;i<canvas.size();++i) assert(canvas[i]==0xCC);
    auto previous=panel;
    memset(game.data(),0,game.size());paperboy_landscape_game(game.data(),panel.data());
    for(int py=0;py<540;++py) for(int px=0;px<960;++px) {
      if(px>=240 && px<720 && py>=54 && py<486) assert(pixel(panel,px,py));
      else assert(pixel(panel,px,py)==pixel(previous,px,py));
    }

    paperboy_landscape_set_fullscreen(true);
    assert(paperboy_landscape_fullscreen());
    auto exit_touch=point(480,270);
    assert(paperboy_landscape_buttons(&exit_touch)==0);
    assert(paperboy_landscape_actions(&exit_touch)==PAPERBOY_ACTION_FULLSCREEN);
    std::fill(game.begin(),game.end(),0xFF);
    game[0]=0x7F;
    std::fill(panel.begin(),panel.end(),0xCC);
    paperboy_landscape_draw(canvas.data(),panel.data(),game.data(),0,true,false,nullptr,nullptr,0);
    int black_x=180, black_y=0;
    if(mode) {black_x=959-black_x;black_y=539-black_y;}
    assert(pixel(panel,black_x,black_y));
    int repeated_x=181, repeated_y=1;
    if(mode) {repeated_x=959-repeated_x;repeated_y=539-repeated_y;}
    assert(pixel(panel,repeated_x,repeated_y));
    assert(!pixel(panel,0,270));
    assert(!pixel(panel,959,270));
    for(size_t i=64800;i<panel.size();++i) assert(panel[i]==0xCC);
    std::fill(game.begin(),game.end(),0x00);
    paperboy_landscape_game(game.data(),panel.data());
    assert(pixel(panel,180,0));
    assert(pixel(panel,779,539));
    assert(!pixel(panel,179,270));
    assert(!pixel(panel,780,270));
    paperboy_landscape_set_fullscreen(false);
  }
  paperboy_orientation_cycle();assert(!paperboy_is_landscape());
  puts("PASS: rotations, controls, native landscape and aspect-fit fullscreen bounds");
}
