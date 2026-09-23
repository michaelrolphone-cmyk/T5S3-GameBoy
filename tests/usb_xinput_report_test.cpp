#include <cassert>
#include "usb_xinput_report.h"

int main() {
  assert(usb_xinput_interface(0xff, 0x5d, 1));
  assert(usb_xinput_interface(0xff, 0x5d, 0x81));
  assert(!usb_xinput_interface(3, 0, 0));
  XboxInputReport state;
  const uint8_t wired[20] = {0, 20, 0x18, 0x33, 0, 0,
                              0, 0, 0, 0, 0, 0, 0, 0};
  assert(usb_xinput_decode(wired, sizeof(wired), false, state) == XboxReportKind::Input);
  assert(state.pad.right && state.pad.start && state.pad.a && state.pad.b);
  assert(state.pad.l && state.pad.r && !state.pad.select);
  uint8_t wireless[24] = {0, 1, 0, 0};
  for (size_t i = 0; i < sizeof(wired); ++i) wireless[i + 4] = wired[i];
  assert(usb_xinput_decode(wireless, sizeof(wireless), true, state) == XboxReportKind::Input);
  const uint8_t paired[] = {0x08, 0x80};
  const uint8_t unpaired[] = {0x08, 0x00};
  assert(usb_xinput_decode(paired, sizeof(paired), true, state) == XboxReportKind::Connected);
  assert(usb_xinput_decode(unpaired, sizeof(unpaired), true, state) == XboxReportKind::Disconnected);
  assert(usb_xinput_decode(wireless, 23, true, state) == XboxReportKind::Ignored);
  assert(usb_xinput_decode(wired, 19, false, state) == XboxReportKind::Ignored);
}
