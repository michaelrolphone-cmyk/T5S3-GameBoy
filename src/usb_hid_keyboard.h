#pragma once

#include <stddef.h>
#include <stdint.h>

#include "usb_hid_report.h"

// Key identities are USB HID Keyboard/Keypad usages (page 0x07), not ASCII,
// national-layout characters, or positions in an undocumented vendor packet.
// The entire 0..255 usage space covers standard 104/108-key keyboards,
// modifiers, function/navigation keys and the numeric keypad.
struct UsbHidKeyboardKeys {
  uint32_t usages[8] = {};
};

struct UsbHidKeyboardReport {
  uint8_t report_id = 0;
  uint16_t report_bits = 0;
  uint16_t key_bits[256] = {};  // 0xffff => no variable bit for that usage.
  uint16_t array_bit = 0;
  uint8_t array_width = 0;
  uint8_t array_count = 0;
};

struct UsbHidKeyboardMapping {
  UsbHidGamepadState gamepad;
  uint8_t actions = 0;  // SNES_ACTION_* bits, generated on key/combo edges.
};

// USB HID boot keyboard has eight-byte reports: modifiers, reserved, 6 keys.
// Host must successfully issue SET_PROTOCOL(BOOT) before selecting this layout.
void usb_hid_boot_keyboard_layout(UsbHidKeyboardReport &out);

// Parses a normal report-protocol keyboard (6KRO arrays or NKRO bitmaps).
// Strict report IDs, bounded offsets and keyboard application identity.
bool usb_hid_parse_keyboard_descriptor(const uint8_t *data, size_t length,
                                       UsbHidKeyboardReport &out);
bool usb_hid_decode_keyboard(const UsbHidKeyboardReport &layout,
                             const uint8_t *data, size_t length,
                             UsbHidKeyboardKeys &out);
bool usb_hid_keyboard_pressed(const UsbHidKeyboardKeys &keys, uint8_t usage);
UsbHidKeyboardMapping usb_hid_map_keyboard(const UsbHidKeyboardKeys &current,
                                           const UsbHidKeyboardKeys &previous);
