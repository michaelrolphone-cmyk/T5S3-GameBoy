#pragma once

#include <stdint.h>

// SNES/NES Classic Mini controller on the existing GPIO39/GPIO40 Wire bus,
// 3.3 V and common GND, I2C slave address 0x52 (not an original SNES pad).
// This getter starts a low-priority polling task once; subsequent calls only
// read an atomic cached button mask. It never runs I2C on the emulator frame
// or touches other devices' registers. Returns zero until a valid report and
// immediately after a failed report. Touchscreen input remains independent.
uint8_t snes_mini_controller_buttons();
