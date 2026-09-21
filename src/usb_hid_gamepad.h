#pragma once

#include <stdint.h>

// USB host owns the ESP32-S3 native USB PHY (GPIO19/20). The receiver must
// present a standard HID joystick/gamepad interface in D-input mode.
// The board must have a powered USB OTG connection supplying 5 V VBUS.
// This module never changes the existing battery charger's power configuration.
void usb_hid_gamepad_begin();
uint8_t usb_hid_gamepad_buttons();
uint8_t usb_hid_gamepad_navigation_buttons();
uint8_t usb_hid_gamepad_take_actions();
