#include "usb_hid_keyboard.h"
#include "snes_mini_controller.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

namespace {
const uint8_t kStandard[] = {
  0x05,0x01,0x09,0x06,0xa1,0x01,
  0x05,0x07,0x19,0xe0,0x29,0xe7,0x15,0x00,0x25,0x01,
  0x75,0x01,0x95,0x08,0x81,0x02,
  0x75,0x08,0x95,0x01,0x81,0x01,
  0x05,0x07,0x19,0x00,0x29,0x65,0x15,0x00,0x25,0x65,
  0x75,0x08,0x95,0x06,0x81,0x00,0xc0
};
void key(UsbHidKeyboardKeys &keys, uint8_t usage) {
  keys.usages[usage / 32U] |= uint32_t(1) << (usage % 32U);
}
void check_mapping() {
  UsbHidKeyboardKeys previous = {}, current = {};
  key(current, 0x52); key(current, 0x1d); key(current, 0x1b);
  key(current, 0x2b); key(current, 0x2c); key(current, 0x06);
  UsbHidKeyboardMapping mapped = usb_hid_map_keyboard(current, previous);
  assert(mapped.gamepad.up && mapped.gamepad.a && mapped.gamepad.b);
  assert(mapped.gamepad.select && mapped.gamepad.start && mapped.gamepad.x);
  assert(!mapped.gamepad.y && mapped.actions == 0);
  current = {}; key(current, 0x5e); key(current, 0x5c);
  key(current, 0x5d); key(current, 0x62); key(current, 0x19);
  mapped = usb_hid_map_keyboard(current, previous);
  assert(mapped.gamepad.right && mapped.gamepad.left);
  assert(mapped.gamepad.a && mapped.gamepad.b && mapped.gamepad.y);
  current = {}; key(current, 0x3e); key(current, 0x42);
  key(current, 0x3f); key(current, 0x40); key(current, 0x3a); key(current, 0x3b);
  mapped = usb_hid_map_keyboard(current, previous);
  assert(mapped.actions == (SNES_ACTION_SAVE | SNES_ACTION_LOAD |
      SNES_ACTION_DIM | SNES_ACTION_BRIGHTEN | SNES_ACTION_SETTINGS |
      SNES_ACTION_ROTATE));
  assert(usb_hid_map_keyboard(current, current).actions == 0);
  previous = {}; current = {}; key(current, 0xe0); key(current, 0x16);
  mapped = usb_hid_map_keyboard(current, previous);
  assert((mapped.actions & SNES_ACTION_SAVE) != 0 && !mapped.gamepad.down);
  // Modifier pressed after S is itself a new Ctrl+S edge.
  previous = {}; key(previous, 0x16);
  assert((usb_hid_map_keyboard(current, previous).actions & SNES_ACTION_SAVE) != 0);
  previous = {}; current = {}; key(current, 0x28); key(current, 0x29);
  mapped = usb_hid_map_keyboard(current, previous);
  assert(mapped.gamepad.a && mapped.gamepad.b); // Enter and Escape.
  previous = {}; current = {}; key(current, 0x43); key(current, 0x44);
  mapped = usb_hid_map_keyboard(current, previous);
  assert((mapped.actions & (SNES_ACTION_SETTINGS | SNES_ACTION_ROTATE)) ==
         (SNES_ACTION_SETTINGS | SNES_ACTION_ROTATE));
}
}  // namespace

int main() {
  UsbHidKeyboardReport layout = {};
  UsbHidKeyboardKeys keys = {};
  assert(usb_hid_parse_keyboard_descriptor(kStandard, sizeof(kStandard), layout));
  assert(layout.report_id == 0 && layout.report_bits == 64);
  uint8_t normal[8] = {0x01,0x00,0x1d,0x3e,0x52,0,0,0};
  assert(usb_hid_decode_keyboard(layout, normal, sizeof(normal), keys));
  assert(usb_hid_keyboard_pressed(keys, 0xe0));
  assert(usb_hid_keyboard_pressed(keys, 0x1d));
  assert(usb_hid_keyboard_pressed(keys, 0x3e));
  assert(usb_hid_keyboard_pressed(keys, 0x52));
  normal[2] = 0x01; // ErrorRollOver: release all, never latch.
  assert(!usb_hid_decode_keyboard(layout, normal, sizeof(normal), keys));
  assert(!usb_hid_keyboard_pressed(keys, 0xe0));
  assert(!usb_hid_decode_keyboard(layout, normal, 7, keys));
  usb_hid_boot_keyboard_layout(layout);
  assert(layout.report_bits == 64 && layout.key_bits[0xe7] == 7);
  normal[2] = 0x62; normal[7] = 0x43;
  assert(usb_hid_decode_keyboard(layout, normal, sizeof(normal), keys));
  assert(usb_hid_keyboard_pressed(keys, 0x62));
  assert(usb_hid_keyboard_pressed(keys, 0x43));

  // An explicit report ID changes the payload offset, not the usage map.
  uint8_t id_desc[sizeof(kStandard) + 2] = {};
  for (size_t i = 0; i < 6; ++i) id_desc[i] = kStandard[i];
  id_desc[6] = 0x85; id_desc[7] = 0x07;
  for (size_t i = 6; i < sizeof(kStandard); ++i) id_desc[i + 2] = kStandard[i];
  assert(usb_hid_parse_keyboard_descriptor(id_desc, sizeof(id_desc), layout));
  assert(layout.report_id == 7 && layout.report_bits == 64);
  const uint8_t with_id[9] = {7,0,0,0x3e,0,0,0,0,0};
  assert(usb_hid_decode_keyboard(layout, with_id, sizeof(with_id), keys));
  assert(usb_hid_keyboard_pressed(keys, 0x3e));
  const uint8_t wrong_id[9] = {8,0,0,0x3e,0,0,0,0,0};
  assert(!usb_hid_decode_keyboard(layout, wrong_id, sizeof(wrong_id), keys));

  // 100 one-bit keyboard usages: common NKRO report protocol.
  const uint8_t nkro[] = {0x05,0x01,0x09,0x06,0xa1,0x01,
    0x85,0x02,0x05,0x07,0x19,0x04,0x29,0x67,
    0x15,0x00,0x25,0x01,0x75,0x01,0x95,0x64,0x81,0x02,0xc0};
  assert(usb_hid_parse_keyboard_descriptor(nkro, sizeof(nkro), layout));
  assert(layout.report_id == 2 && layout.report_bits == 100);
  uint8_t nkro_report[14] = {2};
  nkro_report[1 + 25/8] |= 1U << (25%8); // Z usage 0x1d - 4 = 25.
  nkro_report[1 + 92/8] |= 1U << (92%8); // Keypad 8 usage 0x60.
  assert(usb_hid_decode_keyboard(layout, nkro_report, sizeof(nkro_report), keys));
  assert(usb_hid_keyboard_pressed(keys, 0x1d) && usb_hid_keyboard_pressed(keys, 0x60));
  assert(!usb_hid_decode_keyboard(layout, nkro_report, sizeof(nkro_report)-1, keys));
  const uint8_t mouse[] = {0x05,0x01,0x09,0x02,0xa1,0x01,0xc0};
  assert(!usb_hid_parse_keyboard_descriptor(mouse, sizeof(mouse), layout));
  const uint8_t malformed[] = {0x05,0x01,0x09,0x06,0xa1,0x01,0x75};
  assert(!usb_hid_parse_keyboard_descriptor(malformed, sizeof(malformed), layout));
  check_mapping();
  puts("PASS: 108-key usages, boot/6KRO, report ID/NKRO, rollover, shortcuts and menu controls");
  return 0;
}
