#include <cassert>
#include <cstdint>
#include <initializer_list>
#include <vector>
#include <utility>
#include "gbemu.h"
#include "paperboy_ui.h"
#include "snes_mini_controller.h"
#include "paperboy_landscape.h"
#include "t5s3_epd_pins.h"

// The input/dirty-region/render blocks below come from the real staged app.
// Count display work instead of substituting a model of its render policy.
static uint8_t pad, touch_mask, shortcuts;
static uint16_t light_tenths = 50;
static uint8_t last_buttons, last_touch_buttons, full_scene_syncs;
static uint8_t skipped_since_render, rendered_touch;
static uint32_t menu_action, now_ms, rendered_frames, skipped_frames;
static unsigned full_compositions, game_compositions, full_submissions, saves, loads;
static unsigned brightness_updates, settings_requests;
static unsigned pacer_resets;
static bool boot_pressed, last_boot_pressed, g_boot_refresh_irq, boot_refresh_armed = true;
static uint32_t last_boot_refresh_ms;
static std::vector<std::pair<bool, uint8_t>> clear_frames;
static bool touch_ok = true, power_on = true, submit_ok = true, landscape;
static bool landscape_fullscreen;
static PaperboyPage page = PaperboyPage::Game;
static PaperboyPage next_page = PaperboyPage::Game;
static touch_state_t touch{};
static PaperboyBatteryStatus battery{};
static uint8_t buffer[1], g_game_frame[1];
static int compose_timing, draw_timing, flip_timing;
static struct { uint32_t draw_us; } frame_stats{};
static int game_frame_pacer;
static unsigned page_changes;
static bool audio_paused;
constexpr const char *kTag = "frame-test";
template<typename... Args> static void test_log(Args...) {}
#define ESP_LOGI test_log
bool battery_read_status(PaperboyBatteryStatus &) { return true; }
void usb_hid_gamepad_test_active(bool) {}
static void audio_set_paused(bool paused) { audio_paused = paused; }
void paperboy_ui_on_page_changed() { ++page_changes; }
static void reset_game_frame_pacer(int &) { ++pacer_resets; }
constexpr uint8_t kPanelBufferCount = 2;
constexpr int kGameDirtyY = PAPERBOY_LOGICAL_WIDTH - PAPERBOY_GAME_X - GBEMU_FRAME_WIDTH;
constexpr int kGameDirtyHeight = GBEMU_FRAME_WIDTH;
// REFRESH_CONSTANTS
constexpr int LOW = 0;
static int digitalRead(int) { return boot_pressed ? LOW : 1; }
static uint32_t millis() { return now_ms; }
static void vTaskDelay(unsigned) { assert(false); }
static bool epd_video_submit_pending() { return false; }
static void submit_clear_frame(bool white, uint8_t frames) { clear_frames.emplace_back(white, frames); }
uint8_t snes_mini_controller_buttons() { return pad; }
uint8_t snes_mini_controller_take_actions() { return shortcuts; }
uint8_t snes_mini_controller_navigation_buttons() { return pad; }
uint32_t paperboy_ui_map_controller(uint8_t, PaperboyPage, uint32_t) { return menu_action; }
bool paperboy_ui_controller_ready() { return true; }
uint8_t paperboy_ui_map_buttons(const touch_state_t *) { return touch_mask; }
uint32_t paperboy_ui_map_actions(const touch_state_t *, PaperboyPage) { return 0; }
static bool night_light_adjust_brightness(bool brighter) {
  if (brighter) {
    light_tenths += light_tenths < 10U ? 1U : 10U;
    if (light_tenths > 100U) light_tenths = 100U;
  } else if (light_tenths > 0U) {
    light_tenths -= light_tenths <= 10U ? 1U : 10U;
  }
  ++brightness_updates;
  return true;
}
static uint8_t *epd_video_get_backbuffer() { return buffer; }
static int64_t esp_timer_get_time() { return now_ms * 1000; }
static void add_sample(int &, uint32_t) {}
static void compose_scene(uint8_t *, uint8_t buttons, bool, PaperboyPage, const PaperboyBatteryStatus *) {
  ++full_compositions; rendered_touch = buttons;
}
static void rotate_game_to_panel(const uint8_t *, uint8_t *) { ++game_compositions; }
bool paperboy_is_landscape() { return landscape; }
bool paperboy_landscape_fullscreen() { return landscape_fullscreen; }
static bool epd_video_submit(int y, int height) {
  if (y == 0 && height == t5s3_epd::kActiveHeight) ++full_submissions;
  else assert(height == int(
      landscape
          ? (landscape_fullscreen
              ? PAPERBOY_LANDSCAPE_FULLSCREEN_HEIGHT
              : GBEMU_FRAME_HEIGHT)
          : kGameDirtyHeight));
  return submit_ok;
}
// REFRESH_FUNCTIONS
static uint8_t frame() {
  // FRAME_INPUT
  if (actions & PAPERBOY_ACTION_SAVE) ++saves;
  if (actions & PAPERBOY_ACTION_LOAD) ++loads;
  if (actions & PAPERBOY_ACTION_SETTINGS) ++settings_requests;
  // FRAME_PAGE_TRANSITION
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
  shortcuts = SNES_ACTION_BRIGHTEN; frame(); assert(light_tenths == 60);
  shortcuts = SNES_ACTION_DIM; frame(); assert(light_tenths == 50 && brightness_updates == 2);
  light_tenths = 10;
  shortcuts = SNES_ACTION_DIM; frame(); assert(light_tenths == 9);
  shortcuts = SNES_ACTION_BRIGHTEN; frame(); assert(light_tenths == 10 && brightness_updates == 4);
  shortcuts = SNES_ACTION_SAVE; frame(); assert(saves == 1);
  shortcuts = SNES_ACTION_LOAD; frame(); assert(loads == 1);
  shortcuts = SNES_ACTION_SETTINGS; frame(); assert(settings_requests == 1);
  assert(saves == 1 && loads == 1);
  shortcuts = 0; page = next_page = PaperboyPage::Settings; menu_action = PAPERBOY_ACTION_REFRESH;
  assert(frame() == 0 && full_scene_syncs == 1);
  // Menu confirmation/back keys cannot leak into gameplay on a page switch.
  menu_action = 0; next_page = PaperboyPage::Game; pad = GBEMU_INPUT_B;
  assert(frame() == 0 && page == PaperboyPage::Game && !audio_paused);
  next_page = PaperboyPage::Settings; pad = GBEMU_INPUT_A;
  assert(frame() == 0 && page == PaperboyPage::Settings && audio_paused);
  assert(page_changes == 2 && full_scene_syncs == 1);
  // Both sources execute the actual white/black/white clear and redraw each
  // panel buffer on any page. Simultaneous hardware/pad requests clear once.
  menu_action = 0; pad = touch_mask = 0;
  const std::vector<std::pair<bool, uint8_t>> expected_clear = {
      {true, kClearWhiteFrames}, {false, kClearBlackFrames}, {true, kClearWhiteFrames}};
  for (auto current_page : {PaperboyPage::Game, PaperboyPage::Settings}) {
    page = next_page = current_page;
    for (unsigned source : {1u, 2u, 3u}) {
      shortcuts = 0; boot_pressed = false; frame(); // Release/rearm hardware.
      now_ms += kBootDebounceMs;
      shortcuts = (source & 1u) ? SNES_ACTION_REFRESH : 0;
      boot_pressed = (source & 2u) != 0;
      const unsigned compositions = full_compositions, submissions = full_submissions;
      const unsigned resets = pacer_resets;
      full_scene_syncs = 2; skipped_since_render = 5; clear_frames.clear();
      frame();
      assert(clear_frames == expected_clear);
      assert(full_compositions == compositions + kPanelBufferCount);
      assert(full_submissions == submissions + kPanelBufferCount);
      assert(full_scene_syncs == 0 && skipped_since_render == 0);
      assert(pacer_resets == resets + 1);
      shortcuts = 0; frame(); // Held hardware also stays one-shot.
      assert(clear_frames == expected_clear);
    }
  }
  boot_pressed = false; shortcuts = 0; frame();
  clear_frames.clear(); shortcuts = SNES_ACTION_REFRESH | SNES_ACTION_SETTINGS;
  frame(); assert(clear_frames.empty()); // Settings has priority across sources.
}
