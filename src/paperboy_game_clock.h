#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void paperboy_game_clock_sync(uint32_t rtc_epoch_seconds, int64_t monotonic_us);
/* Refresh cached clock text. Returns true only when the displayed minute/text changed. */
bool paperboy_game_clock_update(int64_t monotonic_us);
const char *paperboy_game_clock_label(int64_t monotonic_us);
/* Draw the already formatted clock into caller-owned UI/chrome. Coordinates,
 * pitch and dimensions are explicit so this function never assumes the Game
 * Boy emulated framebuffer. */
void paperboy_game_clock_draw_ui(
    uint8_t *framebuffer,
    size_t pitch,
    int width,
    int height,
    int x,
    int y,
    uint8_t scale,
    bool white,
    int64_t monotonic_us);
