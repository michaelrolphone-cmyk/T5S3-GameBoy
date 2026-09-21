#pragma once

#include <stddef.h>
#include <stdint.h>

// USB HID usages, not guessed offsets or vendor/product IDs. Unsupported
// receiver reports are rejected instead of interpreting arbitrary bytes as keys.
struct UsbHidField {
  uint16_t bit = 0;
  uint8_t width = 0;
  int32_t minimum = 0;
  int32_t maximum = 0;
  bool present = false;
};

struct UsbHidGamepadReport {
  uint8_t report_id = 0;
  uint16_t report_bits = 0;
  UsbHidField x;
  UsbHidField y;
  UsbHidField hat;
  UsbHidField buttons[16]; // HID Button usages 1..16.
};

struct UsbHidGamepadState {
  bool up = false, down = false, left = false, right = false;
  bool a = false, b = false, x = false, y = false;
  bool l = false, r = false, start = false, select = false;
};

bool usb_hid_parse_gamepad_descriptor(const uint8_t *data, size_t length,
                                      UsbHidGamepadReport &out);
bool usb_hid_decode_gamepad(const UsbHidGamepadReport &layout,
                            const uint8_t *report, size_t length,
                            UsbHidGamepadState &out);
