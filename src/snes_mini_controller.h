#pragma once

#include <stdint.h>

// Optional SNES/NES Classic Mini controller on the *existing* board I2C bus:
// GPIO39 SDA, GPIO40 SCL, 3.3 V, common ground, slave address 0x52.
// This module never initializes/reconfigures Wire or touches other I2C slaves.
// Safe to call once per emulated frame. Returns zero when absent or on error;
// retries after disconnection and supports simultaneous touchscreen input.
uint8_t snes_mini_controller_buttons();
