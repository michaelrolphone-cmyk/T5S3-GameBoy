#include <cassert>
#include <cstdint>
#include <initializer_list>
#include "gbemu.h"
#include "paperboy_ui.h"
#include "snes_mini_controller.h"
#include "paperboy_landscape.h"
#include "t5s3_epd_pins.h"

// The input/dirty-region/render blocks below come from the real staged app.
// Count display work instead of substituting a model of its render policy.
static uint8_t pad, touch_mask, shortcuts, light = 5;
static uint8_t last_buttons, last_touch_buttons, full_scene_syncs;
static uint8_t skipped_since_render, rendered_touch;
static uint32_t menu_action, now_ms, rendered_frames, skipped_frames;
static unsigned full_compositions, game_compositions, full_submissions, saves, loads;
static unsigned brightness_updates, settings_requests;
static bool touch_ok = true, power_on = true, submit_ok = true, landscape;
static PaperboyPage page = PaperboyPage::Game;
static touch_state_t touch{};
static PaperboyBatteryStatus battery{};
static uint8_t buffer[1], g_game_frame[1];
static int compose_timing, draw_timing, flip_timing;
static struct { uint32_t draw_us; } frame_stats{};
constexpr uint8_t kPanelBufferCount = 2;
constexpr int kGameDirtyY = PAPERBOY_LOGICAL_WIDTH - PAPERBOY_GAME_X - GBEMU_FRAME_WIDTH;
constexpr int kGameDirtyHeight = GBEMU_FRAME_WIDTH;
uint8_t snes_mini_controller_buttons() { return pad; }
uint8_t snes_mini_controller_take_actions() { return shortcuts; }
uint8_t snes_mini_controller_navigation_buttons() { return pad; }
uint32_t paperboy_ui_map_controller(uint8_t, PaperboyPage, uint32_t) { return menu_action; }
bool paperboy_ui_controller_ready() { return true; }
uint8_t paperboy_ui_map_buttons(const touch_state_t *) { return touch_mask; }
uint32_t paperboy_ui_map_actions(const touch_state_t *, PaperboyPage) { return 0; }
static uint8_t night_light_brightness() { return light; }
static bool night_light_set_brightness(uint8_t value) { light = value; ++brightness_updates; return true; }
static uint8_t *epd_video_get_backbuffer() { return buffer; }
static int64_t esp_timer_get_time() { return now_ms * 1000; }
static void add_sample(int &, uint32_t) {}
static void compose_scene(uint8_t *, uint8_t buttons, bool, PaperboyPage, const PaperboyBatteryStatus *) {
  ++full_compositions; rendered_touch = buttons;
}
static void rotate_game_to_panel(const uint8_t *, uint8_t *) { ++game_compositions; }
bool paperboy_is_landscape() { return landscape; }
static bool epd_video_submit(int y, int height) {
  if (y == 0 && height == t5s3_epd::kActiveHeight) ++full_submissions;
  else assert(height == int(landscape ? GBEMU_FRAME_HEIGHT : kGameDirtyHeight));
  return submit_ok;
}
static uint8_t frame() {
  // FRAME_INPUT
  if (actions & PAPERBOY_ACTION_SAVE) ++saves;
  if (actions & PAPERBOY_ACTION_LOAD) ++loads;
  if (actions & PAPERBOY_ACTION_SETTINGS) ++settings_requests;
  // FRAME_RENDER
  last_buttons = buttons;
  last_touch_buttons = touch_mask;
  return buttons;
}
int main() {
  for (bool wide : {false, true}) {
    landscape = wide;
    for (unsigned i = 0; i < 600; ++i) {
      pad = uint8_t(i); // Includes directions, A/B, Start/Select and turbo pulses.
      assert(frame() == pad);
    }
    assert(full_compositions == 0 && full_submissions == 0);
  }
  assert(game_compositions == 1200);
  // Touch highlights still repaint both buffers and preserve combined input.
  pad = GBEMU_INPUT_A; touch_mask = GBEMU_INPUT_RIGHT;
  assert(frame() == (GBEMU_INPUT_A | GBEMU_INPUT_RIGHT));
  assert(rendered_touch == GBEMU_INPUT_RIGHT && full_scene_syncs == 1);
  frame(); assert(full_scene_syncs == 0);
  touch_mask = 0; frame(); frame();
  assert(rendered_touch == 0 && full_compositions == 4);
  // A busy display cannot drop a pending scene refresh.
  full_scene_syncs = 2; submit_ok = false; frame(); assert(full_scene_syncs == 2);
  submit_ok = true; frame(); frame(); assert(full_scene_syncs == 0);
  // The exact app input block routes shortcuts to their real UI action flags.
  shortcuts = SNES_ACTION_BRIGHTEN; frame(); assert(light == 6);
  shortcuts = SNES_ACTION_DIM; frame(); assert(light == 5 && brightness_updates == 2);
  shortcuts = SNES_ACTION_SAVE; frame(); assert(saves == 1);
  shortcuts = SNES_ACTION_LOAD; frame(); assert(loads == 1);
  shortcuts = SNES_ACTION_SETTINGS; frame(); assert(settings_requests == 1);
  assert(saves == 1 && loads == 1);
  shortcuts = 0; page = PaperboyPage::Settings; menu_action = PAPERBOY_ACTION_REFRESH;
  assert(frame() == 0 && full_scene_syncs == 1);
}
