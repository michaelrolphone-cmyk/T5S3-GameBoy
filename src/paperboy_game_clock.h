#pragma once

#include <stdint.h>

void paperboy_game_clock_sync(uint32_t rtc_epoch_seconds, int64_t monotonic_us);
const char *paperboy_game_clock_label(int64_t monotonic_us);
void paperboy_game_clock_draw(
    uint8_t *framebuffer,
    int64_t monotonic_us,
    bool low_battery);
