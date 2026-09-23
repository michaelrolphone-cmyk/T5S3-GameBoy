#include "usb_hid_gamepad.h"

void usb_hid_gamepad_begin() {}
UsbGamepadTestStatus usb_hid_gamepad_test_status() { return {}; }
void usb_hid_gamepad_test_active(bool) {}
uint8_t usb_hid_gamepad_buttons() { return 0; }
uint8_t usb_hid_gamepad_navigation_buttons() { return 0; }
uint8_t usb_hid_gamepad_take_actions() { return 0; }
