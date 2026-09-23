#pragma once

#include <stddef.h>
#include <stdint.h>
#include "usb_hid_report.h"

enum class XboxReportKind : uint8_t { Ignored, Connected, Disconnected, Input };

inline bool usb_xinput_interface(uint8_t device_class, uint8_t subclass,
                                 uint8_t protocol) {
  return device_class == 0xff && subclass == 0x5d &&
      (protocol == 0x01 || protocol == 0x81);
}

struct XboxInputReport {
  UsbHidGamepadState pad;
  uint16_t buttons = 0;
  int16_t x = 0, y = 0, rx = 0, ry = 0;
};

// Xbox 360 XUSB wired reports are 20 bytes. Wireless receiver reports have a
// four-byte transport prefix and send presence changes without pad data.
inline XboxReportKind usb_xinput_decode(const uint8_t *bytes, size_t length,
                                         bool wireless, XboxInputReport &out) {
  if (!bytes) return XboxReportKind::Ignored;
  if (wireless) {
    if (length < 2) return XboxReportKind::Ignored;
    if ((bytes[0] & 0x08) && !(bytes[1] & 0x80))
      return XboxReportKind::Disconnected;
    if (bytes[1] != 0x01) return (bytes[0] & 0x08) && (bytes[1] & 0x80)
        ? XboxReportKind::Connected : XboxReportKind::Ignored;
    if (length < 24) return XboxReportKind::Ignored;
    bytes += 4;
    length -= 4;
  }
  if (length < 20 || bytes[0] != 0 || bytes[1] < 14)
    return XboxReportKind::Ignored;
  auto axis = [](const uint8_t *data) -> int16_t {
    return static_cast<int16_t>(uint16_t(data[0]) | (uint16_t(data[1]) << 8));
  };
  const uint8_t dpad = bytes[2], buttons = bytes[3];
  out = {};
  out.x = axis(bytes + 6); out.y = static_cast<int16_t>(~axis(bytes + 8));
  out.rx = axis(bytes + 10); out.ry = static_cast<int16_t>(~axis(bytes + 12));
  out.buttons = ((buttons & 0x20) ? 1U : 0U) | ((buttons & 0x10) ? 2U : 0U) |
      ((buttons & 0x80) ? 4U : 0U) | ((buttons & 0x40) ? 8U : 0U) |
      ((buttons & 1) ? 16U : 0U) | ((buttons & 2) ? 32U : 0U) |
      ((dpad & 0x20) ? 256U : 0U) | ((dpad & 0x10) ? 512U : 0U);
  out.pad.up = (dpad & 1) || out.y < -16384;
  out.pad.down = (dpad & 2) || out.y > 16384;
  out.pad.left = (dpad & 4) || out.x < -16384;
  out.pad.right = (dpad & 8) || out.x > 16384;
  out.pad.start = dpad & 0x10; out.pad.select = dpad & 0x20;
  out.pad.l = buttons & 1; out.pad.r = buttons & 2;
  out.pad.a = buttons & 0x10; out.pad.b = buttons & 0x20;
  out.pad.x = buttons & 0x40; out.pad.y = buttons & 0x80;
  return XboxReportKind::Input;
}
