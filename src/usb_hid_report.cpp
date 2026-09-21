#include "usb_hid_report.h"

#include <string.h>

namespace {
struct Globals {
  uint32_t page = 0;
  int32_t minimum = 0;
  int32_t maximum = 0;
  uint8_t size = 0;
  uint8_t count = 0;
  uint8_t id = 0;
};

uint32_t unsigned_item(const uint8_t *bytes, size_t length) {
  uint32_t value = 0;
  for (size_t i = 0; i < length; ++i) value |= uint32_t(bytes[i]) << (8U * i);
  return value;
}

int32_t signed_item(uint32_t value, size_t length) {
  if (length == 0) return 0;
  if (length == 1) return static_cast<int8_t>(value);
  if (length == 2) return static_cast<int16_t>(value);
  return static_cast<int32_t>(value);
}

uint32_t read_bits(const uint8_t *data, size_t length, const UsbHidField &field) {
  uint32_t value = 0;
  if (!field.present || field.width == 0 || field.width > 16 ||
      uint32_t(field.bit) + field.width > length * 8U) return 0;
  for (uint8_t n = 0; n < field.width; ++n) {
    const uint32_t bit = uint32_t(field.bit) + n;
    if (data[bit / 8U] & (1U << (bit % 8U))) value |= 1UL << n;
  }
  return value;
}

int32_t read_value(const uint8_t *data, size_t length, const UsbHidField &field) {
  const uint32_t value = read_bits(data, length, field);
  if (field.minimum < 0 && field.width && field.width < 32 &&
      (value & (1UL << (field.width - 1U)))) {
    return static_cast<int32_t>(value | (~0UL << field.width));
  }
  return static_cast<int32_t>(value);
}

bool axis_negative(const uint8_t *data, size_t length, const UsbHidField &field) {
  if (!field.present || field.maximum <= field.minimum) return false;
  const int64_t value = read_value(data, length, field);
  return 4 * (value - field.minimum) < (field.maximum - field.minimum);
}
bool axis_positive(const uint8_t *data, size_t length, const UsbHidField &field) {
  if (!field.present || field.maximum <= field.minimum) return false;
  const int64_t value = read_value(data, length, field);
  return 4 * (value - field.minimum) > 3LL * (field.maximum - field.minimum);
}
} // namespace

bool usb_hid_parse_gamepad_descriptor(const uint8_t *data, size_t length,
                                      UsbHidGamepadReport &out) {
  out = {};
  if (data == nullptr || length == 0 || length > 1024) return false;
  Globals g, stack[8];
  uint8_t stack_size = 0;
  uint32_t usages[32] = {};
  uint8_t usage_count = 0;
  uint32_t usage_min = 0, usage_max = 0;
  bool has_range = false;
  uint16_t positions[256] = {};
  unsigned depth = 0, gamepad_depth = 0;
  bool found = false;
  bool report_chosen = false;

  for (size_t p = 0; p < length;) {
    const uint8_t prefix = data[p++];
    if (prefix == 0xFE) { // Long item: length, tag, data.
      if (p + 2 > length || p + 2U + data[p] > length) return false;
      const uint8_t count = data[p];
      p += 2U + count;
      continue;
    }
    const size_t count = (prefix & 3U) == 3U ? 4U : (prefix & 3U);
    if (p + count > length) return false;
    const uint32_t value = unsigned_item(data + p, count);
    const int32_t signed_value = signed_item(value, count);
    p += count;
    const uint8_t kind = (prefix >> 2U) & 3U;
    const uint8_t tag = (prefix >> 4U) & 15U;
    if (kind == 1) { // Global items persist across main items.
      switch (tag) {
        case 0: g.page = value; break;
        case 1: g.minimum = signed_value; break;
        case 2: g.maximum = g.minimum < 0 ? signed_value : static_cast<int32_t>(value); break;
        case 7: if (value > 32) return false; g.size = static_cast<uint8_t>(value); break;
        case 8: if (value == 0 || value > 255) return false; g.id = static_cast<uint8_t>(value); break;
        case 9: if (value > 128) return false; g.count = static_cast<uint8_t>(value); break;
        case 10: if (stack_size >= 8) return false; stack[stack_size++] = g; break;
        case 11: if (!stack_size) return false; g = stack[--stack_size]; break;
        default: break;
      }
    } else if (kind == 2) { // Local usages are reset after every main item.
      if (tag == 0 && usage_count < 32) usages[usage_count++] = value;
      if (tag == 1) { usage_min = value; has_range = true; }
      if (tag == 2) usage_max = value;
    } else if (kind == 0) {
      if (tag == 10) { // Collection (Application: Generic Desktop Gamepad/Joystick).
        const uint32_t usage = usage_count ? usages[0] : usage_min;
        if (depth == 0 && value == 1 && g.page == 1 &&
            (usage == 4 || usage == 5) && !found) {
          gamepad_depth = 1;
          found = true;
        }
        ++depth;
      } else if (tag == 12) {
        if (!depth) return false;
        if (gamepad_depth == depth) gamepad_depth = 0;
        --depth;
      } else if (tag == 8) { // Input: advance offset even for padding/constants.
        const uint32_t bits = uint32_t(g.size) * g.count;
        if (bits > 1024 || uint32_t(positions[g.id]) + bits > 1024) return false;
        const bool can_use = gamepad_depth && !(value & 1U) &&
            (value & 2U) && g.size && g.size <= 16 &&
            (!report_chosen || g.id == out.report_id);
        if (can_use) {
          if (!report_chosen) { out.report_id = g.id; report_chosen = true; }
          for (uint8_t n = 0; n < g.count; ++n) {
            const uint32_t usage = n < usage_count ? usages[n] :
                has_range && usage_min + n <= usage_max ? usage_min + n :
                usage_count ? usages[usage_count - 1U] : 0U;
            UsbHidField *field = nullptr;
            if (g.page == 1) {
              if (usage == 0x30) field = &out.x;
              if (usage == 0x31) field = &out.y;
              if (usage == 0x39) field = &out.hat;
            } else if (g.page == 9 && usage >= 1 && usage <= 16) {
              field = &out.buttons[usage - 1U];
            }
            if (field && !field->present) {
              field->present = true;
              field->bit = static_cast<uint16_t>(positions[g.id] + uint32_t(n) * g.size);
              field->width = g.size;
              field->minimum = g.minimum;
              field->maximum = g.maximum;
            }
          }
        }
        positions[g.id] = static_cast<uint16_t>(positions[g.id] + bits);
      }
      usage_count = 0;
      usage_min = usage_max = 0;
      has_range = false;
    }
  }
  if (!found || !report_chosen || depth || stack_size ||
      !(out.hat.present || out.x.present || out.y.present)) return false;
  out.report_bits = positions[out.report_id];
  return out.report_bits > 0;
}

bool usb_hid_decode_gamepad(const UsbHidGamepadReport &layout,
                            const uint8_t *report, size_t length,
                            UsbHidGamepadState &out) {
  out = {};
  if (!report || !layout.report_bits) return false;
  if (layout.report_id) {
    if (!length || report[0] != layout.report_id) return false;
    ++report;
    --length;
  }
  if (length * 8U < layout.report_bits) return false;
  if (layout.hat.present) {
    const int32_t hat = read_value(report, length, layout.hat) - layout.hat.minimum;
    if (hat >= 0 && hat <= 7) {
      out.up = hat == 0 || hat == 1 || hat == 7;
      out.right = hat >= 1 && hat <= 3;
      out.down = hat >= 3 && hat <= 5;
      out.left = hat >= 5 && hat <= 7;
    }
  } else {
    out.left = axis_negative(report, length, layout.x);
    out.right = axis_positive(report, length, layout.x);
    out.up = axis_negative(report, length, layout.y);
    out.down = axis_positive(report, length, layout.y);
  }
  bool button[16] = {};
  for (unsigned i = 0; i < 16; ++i) {
    if (layout.buttons[i].present)
      button[i] = read_value(report, length, layout.buttons[i]) != 0;
  }
  // Standard USB D-input SNES-style ordering. The USB HID Button usages are
  // 1-based. This mapping is deliberately separate from I2C Mini controllers.
  out.b = button[0]; out.a = button[1];
  out.y = button[2]; out.x = button[3];
  out.l = button[4]; out.r = button[5];
  out.select = button[8]; out.start = button[9];
  return true;
}
