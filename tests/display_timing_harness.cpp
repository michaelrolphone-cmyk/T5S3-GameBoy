#include <cassert>
#include <cstdint>
#include <initializer_list>
#include "epd_video.h"
#include "paperboy_ui.h"
#include "paperboy_storage.h"

// The functions and policy blocks are extracted from the staged application.
static uint8_t g_target_fps = PAPERBOY_DISPLAY_FPS_DEFAULT;
static int lock_depth;
#define portENTER_CRITICAL(lock) (++lock_depth)
#define portEXIT_CRITICAL(lock) (--lock_depth)
static int64_t now_us;
static unsigned yields;
static int64_t esp_timer_get_time() { return now_us; }
static void vTaskDelay(unsigned ticks) { ++yields; now_us += ticks * 1000; }
static void delayMicroseconds(unsigned us) { now_us += us; }
// FPS_ACCESSORS
// SCAN_PACER

constexpr uint8_t kPanelBufferCount = 2;
constexpr uint8_t kMinSkippedFramesBetweenRenders = 1;
static uint8_t full_scene_syncs, skipped_since_render;
static bool ready = true;
bool epd_video_can_submit() { return ready; }
static bool can_render() {
  // RENDER_POLICY
  return !skip_render;
}

static PaperboyStorageConfig g_storage_config;
static bool g_storage_ready = true, save_ok = true;
static unsigned saves, notices;
static int game_frame_pacer;
static bool write_current_config() { ++saves; return save_ok; }
static void set_notice(const char *, unsigned) { ++notices; }
static void reset_game_frame_pacer(int &) {}
static void change(uint32_t actions, PaperboyPage page = PaperboyPage::Display) {
  // DISPLAY_SETTING
}

int main() {
  assert(epd_video_target_fps() == 24);
  assert(!can_render()); // Default keeps alternate-frame rendering.
  skipped_since_render = 1; assert(can_render());
  skipped_since_render = 0;
  now_us = 0; sleep_to_target_frame(0); assert(now_us == 1000000 / 24);

  change(PAPERBOY_ACTION_FPS_UP, PaperboyPage::Game);
  assert(epd_video_target_fps() == 24 && saves == 0);
  change(PAPERBOY_ACTION_FPS_DOWN); assert(epd_video_target_fps() == 24 && saves == 0);
  for (unsigned fps : {30u, 36u, 42u, 48u}) {
    change(PAPERBOY_ACTION_FPS_UP);
    assert(epd_video_target_fps() == fps && g_storage_config.display_fps == fps);
    assert(full_scene_syncs == kPanelBufferCount && skipped_since_render == 0);
    full_scene_syncs = 0;
    assert(can_render()); // Faster modes can render consecutive emulated frames.
    ready = false; assert(!can_render()); ready = true;
    now_us = 0; yields = 0;
    sleep_to_target_frame(0);
    assert(now_us == 1000000 / fps && yields > 0);
    now_us = 50000; yields = 0; // An over-budget scan must still block once.
    sleep_to_target_frame(0);
    assert(now_us == 51000 && yields == 1);
  }
  assert(saves == 4);
  change(PAPERBOY_ACTION_FPS_UP); assert(epd_video_target_fps() == 48 && saves == 4);
  change(PAPERBOY_ACTION_FPS_UP | PAPERBOY_ACTION_FPS_DOWN);
  assert(epd_video_target_fps() == 48 && saves == 4);
  change(PAPERBOY_ACTION_FPS_DOWN); assert(epd_video_target_fps() == 42 && saves == 5);
  change(PAPERBOY_ACTION_FPS_DEFAULT); assert(epd_video_target_fps() == 24 && saves == 6);
  change(PAPERBOY_ACTION_FPS_DEFAULT); assert(saves == 6);
  full_scene_syncs = 0; assert(!can_render());
  full_scene_syncs = 1; assert(can_render()); // UI refresh still overrides the skip.

  save_ok = false; change(PAPERBOY_ACTION_FPS_UP);
  assert(epd_video_target_fps() == 30 && notices == 1 && saves == 7);
  g_storage_ready = false; change(PAPERBOY_ACTION_FPS_UP);
  assert(epd_video_target_fps() == 36 && saves == 7); // Usable without SD.
  for (unsigned fps : {0u, 23u, 25u, 49u, 255u}) {
    assert(!epd_video_set_target_fps(fps));
    assert(epd_video_target_fps() == 36);
  }
  assert(lock_depth == 0);
}
