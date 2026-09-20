#pragma once

#include <stdint.h>

// T5S3 Pro GPIO11 LED boost enable. Brightness is 0..100 percent; 0 is off.
// State is saved to ESP32 NVS so the control also works without an SD card.
void night_light_init();
uint8_t night_light_brightness();
bool night_light_set_brightness(uint8_t percent);
void night_light_shutdown();
