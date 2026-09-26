#include "paperboy_game_clock.h"

#include <limits.h>
#include <stdio.h>

#include "mono_canvas.h"

namespace {

uint32_t g_rtc_epoch_seconds = 0U;
int64_t g_sync_monotonic_us = 0;
uint32_t g_cached_minute = UINT32_MAX;
char g_clock_label[9] = "--:--";

uint32_t current_epoch_seconds(int64_t monotonic_us) {
  if (g_rtc_epoch_seconds == 0U) {
    return 0U;
  }
  const uint64_t elapsed_us = monotonic_us <= g_sync_monotonic_us
      ? 0ULL
      : static_cast<uint64_t>(monotonic_us - g_sync_monotonic_us);
  const uint32_t elapsed_seconds =
      static_cast<uint32_t>(elapsed_us / 1000000ULL);
  return g_rtc_epoch_seconds + elapsed_seconds;
}

bool refresh_label(int64_t monotonic_us) {
  const uint32_t epoch_seconds = current_epoch_seconds(monotonic_us);
  if (epoch_seconds == 0U) {
    const bool changed = g_cached_minute != UINT32_MAX ||
        g_clock_label[0] != '-' || g_clock_label[1] != '-';
    g_cached_minute = UINT32_MAX;
    snprintf(g_clock_label, sizeof(g_clock_label), "--:--");
    return changed;
  }

  const uint32_t minute = epoch_seconds / 60U;
  if (minute == g_cached_minute) {
    return false;
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
  return true;
}

}  // namespace

void paperboy_game_clock_sync(uint32_t rtc_epoch_seconds, int64_t monotonic_us) {
  g_rtc_epoch_seconds = rtc_epoch_seconds;
  g_sync_monotonic_us = monotonic_us;
  g_cached_minute = UINT32_MAX;
  refresh_label(monotonic_us);
}

bool paperboy_game_clock_update(int64_t monotonic_us) {
  return refresh_label(monotonic_us);
}

const char *paperboy_game_clock_label(int64_t monotonic_us) {
  refresh_label(monotonic_us);
  return g_clock_label;
}

void paperboy_game_clock_draw_ui(
    uint8_t *framebuffer,
    size_t pitch,
    int width,
    int height,
    int x,
    int y,
    uint8_t scale,
    bool white,
    int64_t monotonic_us) {
  if (!framebuffer || !pitch || width <= 0 || height <= 0 || scale == 0U) {
    return;
  }
  mono_draw_text(
      framebuffer,
      pitch,
      width,
      height,
      x,
      y,
      paperboy_game_clock_label(monotonic_us),
      scale,
      white);
}
