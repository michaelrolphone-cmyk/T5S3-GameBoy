#pragma once

#include <stdint.h>

// T5S3 Pro GPIO11 LED boost enable.
//
// Brightness is represented internally in tenths of one percent:
//   0 = off, 1 = 0.1%, 10 = 1.0%, 100 = 10.0%.
// 10.0% remains the maximum allowed brightness.
//
// The legacy whole-percent getters/setters remain for source compatibility.
// State is saved to ESP32 NVS so the control also works without an SD card.
void night_light_init();

uint16_t night_light_brightness_tenths();
bool night_light_set_brightness_tenths(uint16_t tenths_percent);

// One user-visible adjustment. Above 1.0% this changes brightness by 1.0%.
// From 0.0% through 1.0% it changes brightness by 0.1%.
bool night_light_adjust_brightness(bool brighter);

// Legacy whole-percent compatibility API.
uint8_t night_light_brightness();
bool night_light_set_brightness(uint8_t percent);

void night_light_shutdown();
