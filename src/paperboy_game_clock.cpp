#include "paperboy_game_clock.h"

#include <limits.h>
#include <stdio.h>

#include "gbemu.h"
#include "mono_canvas.h"

namespace {

constexpr int kClockBoxWidth = 54;
constexpr int kClockBoxHeight = 13;
constexpr int kClockX = GBEMU_FRAME_WIDTH - kClockBoxWidth - 4;
constexpr int kClockNormalY = 4;
constexpr int kClockLowBatteryY = 42;

uint32_t g_rtc_epoch_seconds = 0U;
int64_t g_sync_monotonic_us = 0;
uint32_t g_cached_minute = UINT32_MAX;
char g_clock_label[9] = "--:--";

uint32_t current_epoch_seconds(int64_t monotonic_us) {
  if (g_rtc_epoch_seconds == 0U) {
    return 0U;
  }
  const int64_t elapsed_us = monotonic_us - g_sync_monotonic_us;
  const uint32_t elapsed_seconds = elapsed_us <= 0
      ? 0U
      : static_cast<uint32_t>(elapsed_us / 1000000LL);
  return g_rtc_epoch_seconds + elapsed_seconds;
}

void refresh_label(int64_t monotonic_us) {
  const uint32_t epoch_seconds = current_epoch_seconds(monotonic_us);
  if (epoch_seconds == 0U) {
    g_cached_minute = UINT32_MAX;
    snprintf(g_clock_label, sizeof(g_clock_label), "--:--");
    return;
  }

  const uint32_t minute = epoch_seconds / 60U;
  if (minute == g_cached_minute) {
    return;
  }
  g_cached_minute = minute;

  const uint32_t minutes_today = minute % (24U * 60U);
  const uint32_t hour24 = minutes_today / 60U;
  const uint32_t minute_of_hour = minutes_today % 60U;
  const uint32_t hour12 = hour24 % 12U == 0U ? 12U : hour24 % 12U;
  const char *suffix = hour24 < 12U ? "AM" : "PM";
  snprintf(
      g_clock_label,
      sizeof(g_clock_label),
      "%02lu:%02lu %s",
      static_cast<unsigned long>(hour12),
      static_cast<unsigned long>(minute_of_hour),
      suffix);
}

}  // namespace

void paperboy_game_clock_sync(uint32_t rtc_epoch_seconds, int64_t monotonic_us) {
  g_rtc_epoch_seconds = rtc_epoch_seconds;
  g_sync_monotonic_us = monotonic_us;
  g_cached_minute = UINT32_MAX;
  refresh_label(monotonic_us);
}

const char *paperboy_game_clock_label(int64_t monotonic_us) {
  refresh_label(monotonic_us);
  return g_clock_label;
}

void paperboy_game_clock_draw(
    uint8_t *framebuffer,
    int64_t monotonic_us,
    bool low_battery) {
  if (framebuffer == nullptr) {
    return;
  }

  const char *label = paperboy_game_clock_label(monotonic_us);
  const int y = low_battery ? kClockLowBatteryY : kClockNormalY;
  mono_fill_rect(
      framebuffer,
      GBEMU_FRAME_PITCH_BYTES,
      GBEMU_FRAME_WIDTH,
      GBEMU_FRAME_HEIGHT,
      kClockX,
      y,
      kClockBoxWidth,
      kClockBoxHeight,
      true);
  mono_draw_frame(
      framebuffer,
      GBEMU_FRAME_PITCH_BYTES,
      GBEMU_FRAME_WIDTH,
      GBEMU_FRAME_HEIGHT,
      kClockX,
      y,
      kClockBoxWidth,
      kClockBoxHeight,
      1,
      false);
  mono_draw_text(
      framebuffer,
      GBEMU_FRAME_PITCH_BYTES,
      GBEMU_FRAME_WIDTH,
      GBEMU_FRAME_HEIGHT,
      kClockX + 3,
      y + 3,
      label,
      1,
      false);
}
