#include "paperboy_game_clock.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

#include <algorithm>
#include <vector>

namespace {

bool any_changed(const std::vector<uint8_t> &buffer, uint8_t initial, size_t bytes) {
  for (size_t i = 0; i < bytes; ++i) {
    if (buffer[i] != initial) return true;
  }
  return false;
}

void assert_region_value(const std::vector<uint8_t> &buffer, size_t pitch,
                         int x, int y, int width, int height, uint8_t value) {
  const int first_byte = x / 8;
  const int last_byte = (x + width - 1) / 8;
  for (int row = y; row < y + height; ++row) {
    for (int byte = first_byte; byte <= last_byte; ++byte) {
      assert(buffer[static_cast<size_t>(row) * pitch + static_cast<size_t>(byte)] == value);
    }
  }
}

}  // namespace

int main() {
  paperboy_game_clock_sync(12U * 3600U + 34U * 60U + 56U, 1000000LL);
  assert(strcmp(paperboy_game_clock_label(1000000LL), "12:34 PM") == 0);
  assert(!paperboy_game_clock_update(4999999LL));
  assert(paperboy_game_clock_update(5000000LL));
  assert(strcmp(paperboy_game_clock_label(5000000LL), "12:35 PM") == 0);
  assert(!paperboy_game_clock_update(5000001LL));

  paperboy_game_clock_sync(23U * 3600U + 59U * 60U + 50U, 0);
  assert(strcmp(paperboy_game_clock_label(15000000LL), "12:00 AM") == 0);

  paperboy_game_clock_sync(8U * 3600U + 7U * 60U, 0);
  assert(strcmp(paperboy_game_clock_label(0), "08:07 AM") == 0);

  paperboy_game_clock_sync(0U, 0);
  assert(strcmp(paperboy_game_clock_label(60000000LL), "--:--") == 0);

  // Portrait: the readable 3x clock lives below the 480x432 emulated viewport.
  constexpr int portrait_width = 540;
  constexpr int portrait_height = 960;
  constexpr size_t portrait_pitch = (portrait_width + 7U) / 8U;
  constexpr size_t portrait_bytes = portrait_pitch * portrait_height;
  std::vector<uint8_t> portrait(portrait_bytes + 16U, 0x00U);
  paperboy_game_clock_sync(8U * 3600U + 7U * 60U, 0);
  paperboy_game_clock_draw_ui(
      portrait.data(), portrait_pitch, portrait_width, portrait_height,
      184, 542, 3, true, 0);
  assert(any_changed(portrait, 0x00U, portrait_bytes));
  assert_region_value(portrait, portrait_pitch, 32, 88, 480, 432, 0x00U);
  for (size_t i = portrait_bytes; i < portrait.size(); ++i) assert(portrait[i] == 0x00U);

  // Landscape: the 3x clock lives in the top-center chrome between SAVE/LOAD,
  // outside the 480x432 game rectangle at x=240..719, y=54..485.
  constexpr int landscape_width = 960;
  constexpr int landscape_height = 540;
  constexpr size_t landscape_pitch = landscape_width / 8U;
  constexpr size_t landscape_bytes = landscape_pitch * landscape_height;
  std::vector<uint8_t> landscape(landscape_bytes + 16U, 0xFFU);
  paperboy_game_clock_draw_ui(
      landscape.data(), landscape_pitch, landscape_width, landscape_height,
      430, 16, 3, false, 0);
  assert(any_changed(landscape, 0xFFU, landscape_bytes));
  assert_region_value(landscape, landscape_pitch, 240, 54, 480, 432, 0xFFU);
  for (size_t i = landscape_bytes; i < landscape.size(); ++i) assert(landscape[i] == 0xFFU);

  return 0;
}
