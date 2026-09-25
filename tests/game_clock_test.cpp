#include "paperboy_game_clock.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

#include <vector>

#include "gbemu.h"

int main() {
  paperboy_game_clock_sync(12U * 3600U + 34U * 60U + 56U, 1000000LL);
  assert(strcmp(paperboy_game_clock_label(1000000LL), "12:34") == 0);
  assert(strcmp(paperboy_game_clock_label(4999999LL), "12:34") == 0);
  assert(strcmp(paperboy_game_clock_label(5000000LL), "12:35") == 0);

  paperboy_game_clock_sync(23U * 3600U + 59U * 60U + 50U, 0);
  assert(strcmp(paperboy_game_clock_label(15000000LL), "00:00") == 0);

  paperboy_game_clock_sync(0U, 0);
  assert(strcmp(paperboy_game_clock_label(60000000LL), "--:--") == 0);

  std::vector<uint8_t> frame(GBEMU_FRAMEBUFFER_SIZE + 16U, 0xFFU);
  paperboy_game_clock_sync(8U * 3600U + 7U * 60U, 0);
  paperboy_game_clock_draw(frame.data(), 0, false);
  bool changed = false;
  for (size_t i = 0; i < GBEMU_FRAMEBUFFER_SIZE; ++i) {
    if (frame[i] != 0xFFU) {
      changed = true;
      break;
    }
  }
  assert(changed);
  for (size_t i = GBEMU_FRAMEBUFFER_SIZE; i < frame.size(); ++i) {
    assert(frame[i] == 0xFFU);
  }

  std::fill(frame.begin(), frame.end(), 0xFFU);
  paperboy_game_clock_draw(frame.data(), 0, true);
  for (size_t i = GBEMU_FRAMEBUFFER_SIZE; i < frame.size(); ++i) {
    assert(frame[i] == 0xFFU);
  }
  return 0;
}
