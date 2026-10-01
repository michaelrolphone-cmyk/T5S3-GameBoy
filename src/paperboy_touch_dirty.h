#pragma once

#include <stdint.h>

#include "gbemu.h"

// The video engine refreshes whole scan rows. Return only rows containing
// changed control highlights; the emulator frame is submitted separately.
inline void paperboy_touch_dirty_rows(uint8_t changed, bool landscape,
                                     bool reverse, uint16_t &first,
                                     uint16_t &height) {
  uint16_t lo = 540, hi = 0;
  auto include = [&](uint16_t start, uint16_t end) {
    if (reverse) {
      const uint16_t old_start = start;
      start = static_cast<uint16_t>(539 - end);
      end = static_cast<uint16_t>(539 - old_start);
    }
    if (start < lo) lo = start;
    if (end > hi) hi = end;
  };
  const uint8_t dpad = GBEMU_INPUT_UP | GBEMU_INPUT_DOWN |
                       GBEMU_INPUT_LEFT | GBEMU_INPUT_RIGHT;
  if (landscape) {
    if (changed & dpad) include(174, 366);
    if (changed & GBEMU_INPUT_A) include(189, 271);
    if (changed & GBEMU_INPUT_B) include(285, 367);
    if (changed & (GBEMU_INPUT_SELECT | GBEMU_INPUT_START)) include(430, 480);
  } else {
    // Portrait logical x becomes 539 - panel row. The bounds include the
    // outlines, captions and each control's highlight padding.
    if (changed & dpad) include(297, 498);
    if (changed & GBEMU_INPUT_A) include(59, 143);
    if (changed & GBEMU_INPUT_B) include(143, 227);
    if (changed & GBEMU_INPUT_SELECT) include(285, 382);
    if (changed & GBEMU_INPUT_START) include(157, 254);
  }
  first = lo;
  height = lo <= hi ? static_cast<uint16_t>(hi - lo + 1) : 0;
}
