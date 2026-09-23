#pragma once

#include <stdint.h>

constexpr uint8_t PAPERBOY_DISPLAY_FPS_DEFAULT = 24;
constexpr uint8_t PAPERBOY_DISPLAY_FPS_MAX = 48;
constexpr uint8_t PAPERBOY_DISPLAY_FPS_STEP = 6;

inline bool paperboy_display_fps_valid(long fps) {
  return fps >= PAPERBOY_DISPLAY_FPS_DEFAULT && fps <= PAPERBOY_DISPLAY_FPS_MAX &&
      (fps - PAPERBOY_DISPLAY_FPS_DEFAULT) % PAPERBOY_DISPLAY_FPS_STEP == 0;
}
