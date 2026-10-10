// Host-only reference: compile the original renderers and input mapping unchanged.
// The game pixels and library names below are original synthetic test fixtures.
#include "paperboy_ui.h"
#include "paperboy_landscape.h"
#include "paperboy_game_clock.h"
#include "mono_canvas.h"
#include "../geometry.hpp"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

static uint32_t now_ms = 1000;
static uint16_t light = 50;
uint32_t millis() { return now_ms; }
void night_light_init() {}
uint16_t night_light_brightness_tenths() { return light; }
bool night_light_set_brightness_tenths(uint16_t n) { light = n; return true; }
bool night_light_adjust_brightness(bool up) {
  light = up ? std::min<unsigned>(100, light + (light < 10 ? 1 : 10))
             : (light == 0 ? 0 : light - (light <= 10 ? 1 : 10));
  return true;
}
uint8_t night_light_brightness() { return light / 10; }
bool night_light_set_brightness(uint8_t n) { light = n * 10; return true; }

struct Guarded {
  size_t size;
  std::vector<uint8_t> bytes;
  explicit Guarded(size_t n) : size(n), bytes(n + 32, 0xA5) {}
  uint8_t *data() { return bytes.data() + 16; }
  void verify() const {
    for (size_t i = 0; i < 16; ++i) {
      assert(bytes[i] == 0xA5);
      assert(bytes[size + 16 + i] == 0xA5);
    }
  }
};
struct Image {
  int w, h;
  std::vector<uint8_t> p;
  Image(int width, int height) : w(width), h(height), p(w * h, 255) {}
  uint8_t &at(int x, int y) { return p.at(y * w + x); }
};
struct Fit {
  CapGeometry geometry{};
  int source_w, source_h, w, h, ox, oy, vw, vh;
  explicit Fit(bool wide) : source_w(wide ? 960 : 540), source_h(wide ? 540 : 960),
    w(wide ? 800 : 480), h(wide ? 480 : 800), ox(wide ? 0 : 15), oy(wide ? 15 : 0),
    vw(wide ? 800 : 450), vh(wide ? 450 : 800) {
    assert(geometry.configure(w,h));
    assert(geometry.view_width==800 && geometry.view_height==450);
  }
  bool inverse(int x, int y, int &sx, int &sy) const {
    unsigned px,py;
    if(x<0 || y<0 || !geometry.portrait(x,y,px,py)) return false;
    if(w>h) {sx=py;sy=539-px;} else {sx=px;sy=py;}
    return true;
  }
  int project_x(int x) const { return ox + (x * 5 + 3) / 6; }
  int project_y(int y) const { return oy + (y * 5 + 3) / 6; }
};
static unsigned images, controls, controller_checks, pixel_checks, border_checks, touch_pixels;
static std::string output;
static std::ofstream hitboxes;
static void save(const std::string &name, const Image &im, bool count = true) {
  std::ofstream f(output + "/" + name + ".pgm", std::ios::binary);
  assert(f.good());
  f << "P5\n" << im.w << " " << im.h << "\n255\n";
  f.write(reinterpret_cast<const char *>(im.p.data()), im.p.size());
  assert(f.good()); if (count) ++images;
}
static Image unpack(uint8_t *p, int w, int h, int pitch, bool panel = false) {
  Image im(w, h);
  for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
    bool bit = p[y * pitch + x / 8] & (0x80 >> (x % 8));
    im.at(x, y) = (bit != panel) ? 255 : 0;
  }
  return im;
}
static void render_pair(const std::string &name, const Image &im) {
  save(name + "-original", im);
  Fit fit(im.w > im.h);
  assert(im.w == fit.source_w && im.h == fit.source_h);
  Image scaled(fit.w, fit.h);
  for (int y = 0; y < fit.h; ++y) for (int x = 0; x < fit.w; ++x) {
    int sx, sy;
    if (fit.inverse(x, y, sx, sy)) {
      assert(sx >= 0 && sx < im.w && sy >= 0 && sy < im.h);
      scaled.at(x, y) = im.p[sy * im.w + sx];
      // The original portrait->panel rotation occurs before downsampling.
      // Portrait X therefore reverses the sampling phase by one source pixel.
      assert(sx == (im.w>im.h ? (x-fit.ox)*im.w/fit.vw
                              : im.w-1-(fit.ox+fit.vw-1-x)*im.w/fit.vw));
      assert(sy == (y - fit.oy) * im.h / fit.vh);
      ++pixel_checks;
    } else { assert(scaled.at(x, y) == 255); ++border_checks; }
  }
  save(name + "-x4", scaled);
}
static void orientation(PaperboyOrientation target) {
  while (paperboy_orientation() != target) paperboy_orientation_cycle();
}
static touch_state_t touch(int x, int y, bool wide) {
  touch_state_t t{}; t.touched = true; t.points = 1;
  if (wide) {
    // x/y are electrical display coordinates, including reverse orientation.
    t.x[0] = 539 - y; t.y[0] = x;
  } else { t.x[0] = x; t.y[0] = y; }
  return t;
}
static uint32_t actions(touch_state_t t, PaperboyPage page) {
  paperboy_ui_init(); now_ms += 1000;
  return paperboy_ui_map_actions(&t, page);
}
static void control(const char *name, PaperboyPage page, int x, int y,
                    uint32_t expected, bool button = false, int light_delta = 0) {
  bool wide = page == PaperboyPage::Game && paperboy_is_landscape();
  const bool reverse = wide && paperboy_orientation() == PaperboyOrientation::LandscapeReverse;
  const int physical_x = reverse ? 959 - x : x;
  const int physical_y = reverse ? 539 - y : y;
  Fit fit(wide);
  int tx = fit.project_x(physical_x), ty = fit.project_y(physical_y), sx, sy;
  assert(fit.inverse(tx, ty, sx, sy));
  assert(std::abs(sx - physical_x) <= 1 && std::abs(sy - physical_y) <= 1);
  auto source_touch = touch(physical_x, physical_y, wide), scaled_touch = touch(sx, sy, wide);
  touch_state_t released{}; paperboy_ui_map_buttons(&released);
  light = 50;
  uint32_t original = button ? paperboy_ui_map_buttons(&source_touch) : actions(source_touch, page);
  assert(original == expected); assert(button || int(light) == 50 + light_delta);
  light = 50;
  uint32_t scaled = button ? paperboy_ui_map_buttons(&scaled_touch) : actions(scaled_touch, page);
  assert(scaled == original); assert(button || int(light) == 50 + light_delta);
  hitboxes << int(paperboy_orientation()) << ',' << int(page) << ',' << name << ','
    << physical_x << ',' << physical_y << ',' << tx << ',' << ty << ',' << sx << ',' << sy
    << ',' << original << ',' << scaled << '\n';
  ++controls;
}
static void check_controls() {
  orientation(PaperboyOrientation::Portrait);
  control("A",PaperboyPage::Game,438,646,GBEMU_INPUT_A,true);
  control("B",PaperboyPage::Game,354,720,GBEMU_INPUT_B,true);
  control("Left",PaperboyPage::Game,58,702,GBEMU_INPUT_LEFT,true);
  control("Right",PaperboyPage::Game,226,702,GBEMU_INPUT_RIGHT,true);
  control("Up",PaperboyPage::Game,142,618,GBEMU_INPUT_UP,true);
  control("Down",PaperboyPage::Game,142,786,GBEMU_INPUT_DOWN,true);
  control("Select",PaperboyPage::Game,206,857,GBEMU_INPUT_SELECT,true);
  control("Start",PaperboyPage::Game,334,857,GBEMU_INPUT_START,true);
  control("Power",PaperboyPage::Game,160,41,PAPERBOY_ACTION_POWER);
  control("Save",PaperboyPage::Game,363,41,PAPERBOY_ACTION_SAVE);
  control("Load",PaperboyPage::Game,470,41,PAPERBOY_ACTION_LOAD);
  control("Settings",PaperboyPage::Game,455,923,PAPERBOY_ACTION_SETTINGS);
  control("Rotate",PaperboyPage::Game,99,554,PAPERBOY_ACTION_ROTATE);
  control("Light down",PaperboyPage::Game,380,554,0,false,-10);
  control("Light up",PaperboyPage::Game,470,554,0,false,10);
  control("Battery",PaperboyPage::Settings,270,210,PAPERBOY_ACTION_BATTERY);
  control("Library",PaperboyPage::Settings,270,350,PAPERBOY_ACTION_SD_CARD);
  control("About",PaperboyPage::Settings,270,490,PAPERBOY_ACTION_ABOUT);
  control("Gamepad",PaperboyPage::Settings,270,630,PAPERBOY_ACTION_GAMEPAD_TEST);
  for (auto page : {PaperboyPage::Settings,PaperboyPage::SdCard,PaperboyPage::Battery,
                    PaperboyPage::About,PaperboyPage::GamepadTest}) {
    control("Back",page,76,42,PAPERBOY_ACTION_BACK);
    control("Home",page,464,42,PAPERBOY_ACTION_HOME);
  }
  control("Previous",PaperboyPage::SdCard,98,713,PAPERBOY_ACTION_ROM_PREVIOUS);
  control("Next",PaperboyPage::SdCard,270,713,PAPERBOY_ACTION_ROM_NEXT);
  control("Play",PaperboyPage::SdCard,442,713,PAPERBOY_ACTION_ROM_LAUNCH);
  control("Audio",PaperboyPage::SdCard,270,184,PAPERBOY_ACTION_AUDIO_ENGINE);
  control("Load last",PaperboyPage::SdCard,141,789,PAPERBOY_ACTION_LOAD_LAST);
  control("Rescan",PaperboyPage::SdCard,399,789,PAPERBOY_ACTION_SD_RESCAN);
  control("Refresh",PaperboyPage::Battery,270,876,PAPERBOY_ACTION_REFRESH);
  control("Light off",PaperboyPage::Battery,103,806,PAPERBOY_ACTION_REFRESH,false,-50);
  control("Light down",PaperboyPage::Battery,269,806,PAPERBOY_ACTION_REFRESH,false,-10);
  control("Light up",PaperboyPage::Battery,435,806,PAPERBOY_ACTION_REFRESH,false,10);
  for (auto dir : {PaperboyOrientation::Landscape,PaperboyOrientation::LandscapeReverse}) {
    orientation(dir);
    control("A",PaperboyPage::Game,864,230,GBEMU_INPUT_A,true);
    control("B",PaperboyPage::Game,788,326,GBEMU_INPUT_B,true);
    control("Left",PaperboyPage::Game,38,270,GBEMU_INPUT_LEFT,true);
    control("Right",PaperboyPage::Game,194,270,GBEMU_INPUT_RIGHT,true);
    control("Up",PaperboyPage::Game,116,192,GBEMU_INPUT_UP,true);
    control("Down",PaperboyPage::Game,116,348,GBEMU_INPUT_DOWN,true);
    control("Select",PaperboyPage::Game,112,455,GBEMU_INPUT_SELECT,true);
    control("Start",PaperboyPage::Game,848,455,GBEMU_INPUT_START,true);
    control("Power",PaperboyPage::Game,112,28,PAPERBOY_ACTION_POWER);
    control("Save",PaperboyPage::Game,290,28,PAPERBOY_ACTION_SAVE);
    control("Load",PaperboyPage::Game,670,28,PAPERBOY_ACTION_LOAD);
    control("Rotate",PaperboyPage::Game,848,28,PAPERBOY_ACTION_ROTATE);
    control("Settings",PaperboyPage::Game,480,512,PAPERBOY_ACTION_SETTINGS);
    control("Fullscreen",PaperboyPage::Game,656,512,PAPERBOY_ACTION_FULLSCREEN);
    control("Light down",PaperboyPage::Game,64,92,0,false,-10);
    control("Light up",PaperboyPage::Game,164,92,0,false,10);
  }
  orientation(PaperboyOrientation::Portrait);
  // Original page-change suppression must require release before activation.
  auto held = touch(270,630,false); paperboy_ui_on_page_changed();
  assert(paperboy_ui_map_actions(&held,PaperboyPage::Settings)==0);
  touch_state_t release{}; paperboy_ui_map_actions(&release,PaperboyPage::Settings);
  now_ms += 1000;
  assert(paperboy_ui_map_actions(&held,PaperboyPage::Settings)==PAPERBOY_ACTION_GAMEPAD_TEST);
  assert(paperboy_ui_map_actions(&held,PaperboyPage::Settings)==0);
  paperboy_ui_map_buttons(&release);
  // Actual controller selection reaches each of the four Settings pages.
  paperboy_ui_controller_page_changed();
  assert(paperboy_ui_map_controller(GBEMU_INPUT_A,PaperboyPage::Settings,now_ms)==0);
  paperboy_ui_map_controller(0,PaperboyPage::Settings,++now_ms);
  const uint32_t destinations[] = {PAPERBOY_ACTION_BATTERY,PAPERBOY_ACTION_SD_CARD,
                                  PAPERBOY_ACTION_ABOUT,PAPERBOY_ACTION_GAMEPAD_TEST};
  for (auto destination : destinations) {
    assert(paperboy_ui_map_controller(GBEMU_INPUT_A,PaperboyPage::Settings,++now_ms)==destination);
    paperboy_ui_map_controller(0,PaperboyPage::Settings,++now_ms);
    assert(paperboy_ui_map_controller(GBEMU_INPUT_DOWN,PaperboyPage::Settings,++now_ms)==PAPERBOY_ACTION_REFRESH);
    paperboy_ui_map_controller(0,PaperboyPage::Settings,++now_ms); controller_checks += 2;
  }
}
static void render_button_maps() {
  std::ofstream bounds(output + "/button-bounds.csv");
  bounds << "orientation,button,left,top,right_inclusive,bottom_inclusive,pixels\n";
  const char *names[] = {"A","B","Select","Start","Right","Left","Up","Down"};
  for(auto dir : {PaperboyOrientation::Portrait, PaperboyOrientation::Landscape,
                  PaperboyOrientation::LandscapeReverse}) {
    orientation(dir); const bool wide = paperboy_is_landscape(); Fit fit(wide);
    Image mask(fit.w,fit.h); std::fill(mask.p.begin(),mask.p.end(),0);
    Image source(fit.source_w,fit.source_h),expected(fit.w,fit.h);
    std::fill(expected.p.begin(),expected.p.end(),0);
    for(int y=0;y<source.h;++y) for(int x=0;x<source.w;++x) {
      auto t=touch(x,y,wide);source.at(x,y)=paperboy_ui_map_buttons(&t);
    }
    // Independently reproduce the backend's forward display walk: sample the
    // 960x540 electrical panel, then rotate into reported raw display axes.
    for(unsigned ly=0;ly<fit.geometry.view_height;++ly)
      for(unsigned lx=0;lx<fit.geometry.view_width;++lx) {
        unsigned px=lx*960/fit.geometry.view_width,py=ly*540/fit.geometry.view_height,rx,ry;
        fit.geometry.raw(lx+fit.geometry.x,ly+fit.geometry.y,rx,ry);
        expected.at(rx,ry)=wide?source.at(px,py):source.at(539-py,px);
      }
    int left[8],top[8],right[8],bottom[8],count[8]{};
    std::fill(left,left+8,fit.w);std::fill(top,top+8,fit.h);
    std::fill(right,right+8,-1);std::fill(bottom,bottom+8,-1);
    for(int y=0;y<fit.h;++y) for(int x=0;x<fit.w;++x) {
      int sx,sy;
      if(fit.inverse(x,y,sx,sy)) {
        auto t=touch(sx,sy,wide); mask.at(x,y)=paperboy_ui_map_buttons(&t);
        for(int bit=0;bit<8;++bit) if(mask.at(x,y)&(1U<<bit)) {
          left[bit]=std::min(left[bit],x);top[bit]=std::min(top[bit],y);
          right[bit]=std::max(right[bit],x);bottom[bit]=std::max(bottom[bit],y);++count[bit];
        }
      } else assert(mask.at(x,y)==0);
      // Includes every edge of circular/diagonal/button regions, not only centers.
      assert(mask.at(x,y)==expected.at(x,y));
      ++touch_pixels;
    }
    for(int bit=0;bit<8;++bit) {
      assert(count[bit]>0);
      bounds << int(dir) << ',' << names[bit] << ',' << left[bit] << ',' << top[bit]
             << ',' << right[bit] << ',' << bottom[bit] << ',' << count[bit] << '\n';
    }
    const char *name = dir==PaperboyOrientation::Portrait?"game-portrait":
      dir==PaperboyOrientation::Landscape?"game-landscape":"game-landscape-reverse";
    save(std::string(name)+"-button-mask",mask,false);
    // Original multi-touch union must survive the fit in every orientation.
    int ax=wide?864:438, ay=wide?230:646, lx=wide?38:58, ly=wide?270:702;
    if(dir==PaperboyOrientation::LandscapeReverse) {
      ax=959-ax;ay=539-ay;lx=959-lx;ly=539-ly;
    }
    int sx,sy; assert(fit.inverse(fit.project_x(ax),fit.project_y(ay),sx,sy));
    auto a=touch(sx,sy,wide);
    assert(fit.inverse(fit.project_x(lx),fit.project_y(ly),sx,sy));auto l=touch(sx,sy,wide);
    a.points=2;a.x[1]=l.x[0];a.y[1]=l.y[0];
    assert(paperboy_ui_map_buttons(&a)==(GBEMU_INPUT_A|GBEMU_INPUT_LEFT));
  }
}
int main(int argc, char **argv) {
  assert(argc == 2); output = argv[1];
  hitboxes.open(output + "/hitboxes.csv"); assert(hitboxes.good());
  hitboxes << "orientation,page,control,original_x,original_y,x4_x,x4_y,inverse_x,inverse_y,original_result,x4_result\n";
  paperboy_ui_init();
  paperboy_game_clock_sync(9U*3600U+41U*60U,0);
  Guarded game(GBEMU_FRAMEBUFFER_SIZE), portrait(PAPERBOY_LOGICAL_PITCH*960),
          canvas(120*540), panel(120*540);
  mono_clear(game.data(),game.size,true);
  // Test pattern, not an emulated ROM or commercial screenshot.
  mono_draw_frame(game.data(),60,480,432,10,10,460,412,3,false);
  mono_draw_text(game.data(),60,480,432,58,56,"ORIGINAL UI",4,false);
  mono_draw_text(game.data(),60,480,432,70,100,"TEST PATTERN",3,false);
  for(int y=180;y<350;y+=32) for(int x=40;x<450;x+=32)
    if(((x/32)+(y/32))&1) mono_fill_rect(game.data(),60,480,432,x,y,24,24,false);
  mono_draw_text(game.data(),60,480,432,82,382,"NO ROM LOADED",2,false);
  PaperboyBatteryStatus battery{};
  battery.gauge_found=battery.gauge_read_ok=battery.charger_found=battery.charger_read_ok=true;
  battery.soc_percent=76; battery.voltage_mv=3980; battery.current_ma=-86;
  battery.average_current_ma=-72; battery.remaining_capacity_mah=1140;
  battery.full_capacity_mah=1500; battery.health_percent=98; battery.temperature_dk=2981;
  battery.configured_input_limit_ma=500; battery.active_input_limit_ma=500;
  battery.configured_charge_current_ma=500; battery.configured_charge_voltage_mv=4200;
  battery.system_voltage_mv=3980;
  const char *titles[] = {"OPEN MAZE.GB","TINY TILES.GB","STAR TRAIL.GB",
                         "BYTE RACER.GB","POCKET LIFE.GB","RIVER RUN.GB"};
  PaperboyRomLibraryView library{};
  library.mounted=true; library.rom_count=6; library.selection=2;
  for(unsigned i=0;i<6;++i) library.visible_names[i]=titles[i];
  library.status="SIX SYNTHETIC LIBRARY FIXTURES"; library.audio_engine="CRANKBOY";
  library.has_last_snapshot=true; library.card_size_mb=4096;
  UsbGamepadTestStatus pad{}; pad.provider_ready=pad.connected=true; pad.reports=42;
  pad.buttons=1; pad.hat=8; pad.vid=0x1209; pad.pid=0x0001;
  pad.usb_devices=1; pad.hid_interfaces=1; pad.report_id=1; pad.compact_buttons=true;
  std::strcpy(pad.stage,"HOST REFERENCE FIXTURE");
  std::strcpy(pad.events[0],"SYNTHETIC CONNECTED"); std::strcpy(pad.events[1],"REPORT 42 A PRESSED");
  paperboy_ui_draw_static(portrait.data(),"1.3.18 REF");
  for(unsigned y=0;y<GBEMU_FRAME_HEIGHT;++y) for(unsigned x=0;x<GBEMU_FRAME_WIDTH;++x)
    mono_put_pixel(portrait.data(),68,540,960,PAPERBOY_GAME_X+x,PAPERBOY_GAME_Y+y,
                   game.data()[y*60+x/8] & (0x80>>(x%8)));
  paperboy_ui_draw_dynamic(portrait.data(),0,true,true,&battery,nullptr);
  paperboy_game_clock_draw_ui(portrait.data(),68,540,960,184,542,3,true,0);
  render_pair("game-portrait",unpack(portrait.data(),540,960,68));
  const PaperboyPage pages[] = {PaperboyPage::Settings,PaperboyPage::SdCard,
    PaperboyPage::Battery,PaperboyPage::About,PaperboyPage::GamepadTest};
  const char *names[] = {"settings","library","battery","about","gamepad"};
  for(unsigned i=0;i<5;++i) {
    paperboy_ui_draw_page(portrait.data(),pages[i],&battery,"1.3.18 REF",
                         "TEST PATTERN",true,&library,&pad);
    render_pair(names[i],unpack(portrait.data(),540,960,68));
    portrait.verify();
  }
  Image landscape(960,540);
  for(auto dir : {PaperboyOrientation::Landscape,PaperboyOrientation::LandscapeReverse}) {
    orientation(dir);
    paperboy_landscape_draw(canvas.data(),panel.data(),game.data(),0,true,true,&battery,nullptr,0);
    Image image=unpack(panel.data(),960,540,120,true);
    if(dir==PaperboyOrientation::Landscape) landscape=image;
    else for(int y=0;y<540;++y) for(int x=0;x<960;++x)
      assert(image.at(x,y)==landscape.at(959-x,539-y));
    render_pair(dir==PaperboyOrientation::Landscape?"game-landscape":"game-landscape-reverse",image);
    panel.verify(); canvas.verify();
  }
  orientation(PaperboyOrientation::Landscape);
  paperboy_landscape_set_fullscreen(true);
  paperboy_landscape_draw(canvas.data(),panel.data(),game.data(),0,true,true,&battery,nullptr,0);
  render_pair("game-fullscreen",unpack(panel.data(),960,540,120,true));
  paperboy_landscape_set_fullscreen(false);
  check_controls();
  render_button_maps();
  game.verify(); portrait.verify(); canvas.verify(); panel.verify();
  std::ofstream report(output+"/results.json");
  report << "{\n  \"rendered_images\": " << images
    << ",\n  \"original_to_x4_control_cases\": " << controls
    << ",\n  \"controller_checks\": " << controller_checks
    << ",\n  \"scaled_source_pixels_checked\": " << pixel_checks
    << ",\n  \"letterbox_pixels_checked\": " << border_checks
    << ",\n  \"target_touch_pixels_classified\": " << touch_pixels
    << ",\n  \"two_finger_cases\": 3"
    << ",\n  \"actual_backend_geometry_edge_parity\": true"
    << ",\n  \"reverse_landscape_pixel_parity\": true,\n  \"buffer_canaries\": true,"
       "\n  \"page_change_and_held_tap_suppression\": true\n}\n";
  std::printf("PASS: %u renders, %u mapped controls, %u controller checks, %u source pixels, %u border pixels\n",
              images,controls,controller_checks,pixel_checks,border_checks);
}
