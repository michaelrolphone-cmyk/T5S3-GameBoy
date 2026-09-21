#include "usb_hid_keyboard.h"

#include "snes_mini_controller.h"

namespace {
constexpr uint16_t kAbsent = 0xffffU;
constexpr uint16_t kMaxReportBits = 2048U;

struct Globals {
  uint32_t page = 0;
  uint32_t minimum = 0;
  uint32_t maximum = 0;
  uint8_t size = 0;
  uint16_t count = 0;
  uint8_t id = 0;
};

uint32_t item_value(const uint8_t *bytes, size_t length) {
  uint32_t value = 0;
  for (size_t i = 0; i < length; ++i) value |= uint32_t(bytes[i]) << (8U * i);
  return value;
}

void reset_layout(UsbHidKeyboardReport &layout) {
  layout = {};
  for (unsigned i = 0; i < 256; ++i) layout.key_bits[i] = kAbsent;
}

uint32_t field_value(const uint8_t *data, size_t length, uint16_t start,
                     uint8_t width) {
  if (!width || width > 16 || uint32_t(start) + width > length * 8U) return 0;
  uint32_t result = 0;
  for (uint8_t i = 0; i < width; ++i) {
    const unsigned bit = unsigned(start) + i;
    if (data[bit / 8U] & (1U << (bit % 8U))) result |= uint32_t(1) << i;
  }
  return result;
}

void set_key(UsbHidKeyboardKeys &keys, uint32_t usage) {
  if (usage >= 4 && usage <= 0xe7) {
    keys.usages[usage / 32U] |= uint32_t(1) << (usage % 32U);
  }
}
}  // namespace

void usb_hid_boot_keyboard_layout(UsbHidKeyboardReport &out) {
  reset_layout(out);
  out.report_bits = 64;
  for (uint16_t i = 0; i < 8; ++i) out.key_bits[0xe0U + i] = i;
  out.array_bit = 16;
  out.array_width = 8;
  out.array_count = 6;
}

bool usb_hid_parse_keyboard_descriptor(const uint8_t *data, size_t length,
                                       UsbHidKeyboardReport &out) {
  reset_layout(out);
  if (!data || !length || length > 1024) return false;
  Globals global, stack[8];
  unsigned saved = 0;
  uint32_t usages[32] = {};
  uint8_t usage_count = 0;
  uint32_t usage_min = 0, usage_max = 0;
  bool usage_range = false;
  uint16_t offsets[256] = {};
  unsigned depth = 0, keyboard_depth = 0;
  bool found_keyboard = false, selected = false, has_keys = false;

  for (size_t at = 0; at < length;) {
    const uint8_t prefix = data[at++];
    if (prefix == 0xfe) {
      if (at + 2 > length || at + 2U + data[at] > length) return false;
      at += 2U + data[at];
      continue;
    }
    const size_t size = (prefix & 3U) == 3U ? 4U : (prefix & 3U);
    if (at + size > length) return false;
    const uint32_t value = item_value(data + at, size);
    at += size;
    const uint8_t kind = (prefix >> 2U) & 3U;
    const uint8_t tag = (prefix >> 4U) & 15U;
    if (kind == 1) {
      switch (tag) {
        case 0: global.page = value; break;
        case 1: global.minimum = value; break;
        case 2: global.maximum = value; break;
        case 7: if (value > 32) return false; global.size = static_cast<uint8_t>(value); break;
        case 8: if (!value || value > 255) return false; global.id = static_cast<uint8_t>(value); break;
        case 9: if (value > 255) return false; global.count = static_cast<uint16_t>(value); break;
        case 10: if (saved == 8) return false; stack[saved++] = global; break;
        case 11: if (!saved) return false; global = stack[--saved]; break;
        default: break;
      }
    } else if (kind == 2) {
      if (tag == 0 && usage_count < 32) usages[usage_count++] = value;
      if (tag == 1) { usage_min = value; usage_range = true; }
      if (tag == 2) usage_max = value;
    } else if (kind == 0) {
      if (tag == 10) {
        const uint32_t usage = usage_count ? usages[0] : usage_min;
        if (!depth && value == 1 && global.page == 1 && usage == 6 &&
            !found_keyboard) {
          keyboard_depth = 1;
          found_keyboard = true;
        }
        if (++depth > 16) return false;
      } else if (tag == 12) {
        if (!depth) return false;
        if (keyboard_depth == depth) keyboard_depth = 0;
        --depth;
      } else if (tag == 8) {
        const uint32_t bits = uint32_t(global.size) * global.count;
        if (bits > kMaxReportBits ||
            uint32_t(offsets[global.id]) + bits > kMaxReportBits) return false;
        const bool eligible = keyboard_depth && global.page == 7 &&
            !(value & 1U) && global.size &&
            (!selected || global.id == out.report_id);
        if (eligible) {
          if (!selected) { out.report_id = global.id; selected = true; }
          if ((value & 2U) && global.size == 1) { // Variable NKRO/Modifier bits.
            for (uint16_t i = 0; i < global.count; ++i) {
              const uint32_t usage = i < usage_count ? usages[i] :
                  usage_range && usage_min + i <= usage_max ? usage_min + i :
                  usage_count ? usages[usage_count - 1U] : 0U;
              if (usage < 256 && out.key_bits[usage] == kAbsent) {
                out.key_bits[usage] = uint16_t(offsets[global.id] + i);
                if (usage >= 4 && usage <= 0xe7) has_keys = true;
              }
            }
          } else if (!(value & 2U) && global.size <= 16 &&
                     global.count <= 32 && !out.array_count) {
            // An array carries HID usages, not character codes. Includes
            // normal six-key rollover reports and report-ID keyboards.
            out.array_bit = offsets[global.id];
            out.array_width = global.size;
            out.array_count = static_cast<uint8_t>(global.count);
            has_keys = true;
          }
        }
        offsets[global.id] = uint16_t(offsets[global.id] + bits);
      }
      usage_count = 0;
      usage_min = usage_max = 0;
      usage_range = false;
    }
  }
  if (!found_keyboard || !selected || !has_keys || depth || saved) return false;
  out.report_bits = offsets[out.report_id];
  return out.report_bits != 0;
}

bool usb_hid_decode_keyboard(const UsbHidKeyboardReport &layout,
                             const uint8_t *data, size_t length,
                             UsbHidKeyboardKeys &out) {
  out = {};
  if (!data || !layout.report_bits) return false;
  if (layout.report_id) {
    if (!length || data[0] != layout.report_id) return false;
    ++data;
    --length;
  }
  if (length * 8U < layout.report_bits) return false;
  for (uint16_t usage = 4; usage <= 0xe7; ++usage) {
    if (layout.key_bits[usage] != kAbsent &&
        field_value(data, length, layout.key_bits[usage], 1)) set_key(out, usage);
  }
  for (uint8_t i = 0; i < layout.array_count; ++i) {
    const uint32_t code = field_value(data, length,
        uint16_t(layout.array_bit + uint16_t(i) * layout.array_width),
        layout.array_width);
    if (code >= 1 && code <= 3) { // HID rollover/error: release, never latch.
      out = {};
      return false;
    }
    set_key(out, code);
  }
  return true;
}

bool usb_hid_keyboard_pressed(const UsbHidKeyboardKeys &keys, uint8_t usage) {
  return (keys.usages[usage / 32U] & (uint32_t(1) << (usage % 32U))) != 0;
}

UsbHidKeyboardMapping usb_hid_map_keyboard(const UsbHidKeyboardKeys &current,
                                           const UsbHidKeyboardKeys &previous) {
  UsbHidKeyboardMapping result = {};
  const auto down = [&](uint8_t usage) { return usb_hid_keyboard_pressed(current, usage); };
  const auto was = [&](uint8_t usage) { return usb_hid_keyboard_pressed(previous, usage); };
  const bool ctrl = down(0xe0) || down(0xe4);
  const bool prev_ctrl = was(0xe0) || was(0xe4);
  const auto edge = [&](bool held, bool previously) { return held && !previously; };

  // Arrows, WASD and the dedicated numeric keypad navigation cluster.
  result.gamepad.up = down(0x52) || (!ctrl && down(0x1a)) || down(0x60);
  result.gamepad.down = down(0x51) || (!ctrl && down(0x16)) || down(0x5a);
  result.gamepad.left = down(0x50) || (!ctrl && down(0x04)) || down(0x5c);
  result.gamepad.right = down(0x4f) || (!ctrl && down(0x07)) || down(0x5e);
  // Enter confirms a menu selection; Esc/Backspace returns. Z/X or J/K are
  // gameplay A/B; keypad 5/0 supply another hand position.
  result.gamepad.a = down(0x1d) || down(0x0d) || down(0x28) || down(0x58) || down(0x5d);
  result.gamepad.b = down(0x1b) || down(0x0e) || down(0x29) || down(0x2a) || down(0x62);
  result.gamepad.x = down(0x06); // C = turbo A.
  result.gamepad.y = down(0x19); // V = turbo B.
  result.gamepad.l = down(0x14); // Q = left shoulder.
  result.gamepad.r = down(0x08); // E = right shoulder.
  result.gamepad.select = down(0x2b); // Tab.
  result.gamepad.start = down(0x2c); // Space.

  // Dedicated one-shot commands use key/combo edges, not polling repeats.
  if (edge(down(0x3e) || (ctrl && down(0x16)) || down(0x57),
           was(0x3e) || (prev_ctrl && was(0x16)) || was(0x57)))
    result.actions |= SNES_ACTION_SAVE; // F5, Ctrl+S, keypad +.
  if (edge(down(0x42) || (ctrl && down(0x12)) || down(0x56),
           was(0x42) || (prev_ctrl && was(0x12)) || was(0x56)))
    result.actions |= SNES_ACTION_LOAD; // F9, Ctrl+O, keypad -.
  if (edge(down(0x3f) || (ctrl && down(0x2d)) || down(0x54),
           was(0x3f) || (prev_ctrl && was(0x2d)) || was(0x54)))
    result.actions |= SNES_ACTION_DIM; // F6, Ctrl+-, keypad /.
  if (edge(down(0x40) || (ctrl && down(0x2e)) || down(0x55),
           was(0x40) || (prev_ctrl && was(0x2e)) || was(0x55)))
    result.actions |= SNES_ACTION_BRIGHTEN; // F7, Ctrl+=, keypad *.
  if (edge(down(0x3a) || down(0x43) || (ctrl && down(0x29)),
           was(0x3a) || was(0x43) || (prev_ctrl && was(0x29))))
    result.actions |= SNES_ACTION_SETTINGS; // F1, F10, Ctrl+Esc.
  if (edge(down(0x3b) || down(0x44) || (ctrl && down(0x15)),
           was(0x3b) || was(0x44) || (prev_ctrl && was(0x15))))
    result.actions |= SNES_ACTION_ROTATE; // F2, F11, Ctrl+R.
  return result;
}
