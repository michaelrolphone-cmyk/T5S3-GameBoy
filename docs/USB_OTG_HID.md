# USB OTG HID gamepad support (standalone GameBoy)

This branch runs an ESP-IDF USB host directly on the ESP32-S3's native USB data pins (GPIO19 D-, GPIO20 D+). It does **not** use RiscRTE, an ELF driver, Bluetooth, or the SNES/NES Mini I2C jack. The ZAMPAM B0BWY5CJFR controller is wireless: insert its **USB receiver**, not its charging cable. The firmware discovers HID interfaces and their report descriptors rather than guessing a receiver VID/PID or fixed packet offsets.

## Power and USB role

A host receiver needs a valid **5 V VBUS** supply. This change deliberately leaves the existing BQ25896 charging and OTG boost configuration alone, because `battery_power.cpp` disables boost during initialization and periodically restores the charging profile. For initial hardware testing, use an appropriately designed, externally powered USB OTG hub/adapter which powers the receiver and presents the board as USB host. **Do not inject 5 V into bare ESP32 GPIO, connect two opposed power supplies, or defeat USB-C power/CC negotiation.** A passive USB-C to USB-A adapter alone is not a verified source of 5 V on this board.

The firmware now builds with `ARDUINO_USB_MODE=0` and `ARDUINO_USB_CDC_ON_BOOT=0` so the GameBoy application can claim the native USB PHY. Application USB-CDC serial logging is therefore not available on the same USB connector while the host is running. Recovery/programming through the chip's ROM bootloader remains distinct from application USB CDC; use the board's BOOT/RESET download procedure when needed.

## Receiver compatibility

The USB interface must enumerate as standard HID with a Generic Desktop **Game Pad** or **Joystick** application collection, an interrupt IN endpoint, and a report descriptor describing hat switch or X/Y axes. The firmware queries that descriptor and validates report ID and bit bounds. This is intended for the receiver's **DirectInput/HID** mode. Proprietary XInput/XUSB, vendor-only interfaces, keyboard-emulating receivers, and non-HID reports are not implemented. Exact USB identities and the receiver's physical button numbering have not been confirmed on the user's hardware.

Default SNES-oriented HID usage mapping: Button 1=B, 2=A, 3=Y, 4=X, 5=L, 6=R, 9=Select, 10=Start. The hat or X/Y axes supply D-pad directions. Other USB controllers may use different usage assignments; change the small mapping at the end of `src/usb_hid_report.cpp` after identifying the actual HID reports if necessary.

## Controls

USB input is ORed with the unchanged SNES/NES Mini I2C controller and the existing touchscreen. On the game screen, D-pad and A/B play normally; X pulses A (turbo), Y pulses B. R saves, L loads, Select+R brightens, Select+L dims, L+R+Start+Select opens Settings, and L+R+Right rotates orientation. On menus, D-pad navigates, A selects, and B backs out; the existing UI mapping handles game selection in the SD card view. Button chords consume gameplay input, and unplugging the receiver releases held buttons.

The USB implementation currently consumes **one HID gamepad interface** per receiver. A dual-controller receiver that multiplexes two gamepads through multiple HID report IDs is not yet supported as two independent players; GameBoy is single-player.

## Hardware acceptance test

1. Install the firmware artifact produced by this PR's `GameBoy PlatformIO Build` workflow. Start the board normally and attach the USB receiver through the correctly powered OTG connection. Pair the controller using its manufacturer procedure; select D-input/HID mode if selectable.
2. Look for `usb_gamepad: USB HID host client ready`, `receiver enumerated address=`, `USB receiver VID=... PID=...`, and `HID gamepad active interface=...` in any available serial log. `HID candidate interfaces=0` indicates missing standard HID interface; `interface ... is not a supported gamepad report` indicates incompatible/malformed descriptor. `USB OTG host install failed` indicates the PHY cannot be claimed.
3. Confirm D-pad/A/B, menu navigation, game selection, X/Y turbo, L/R save/load, Select+L/R brightness, Settings chord, and orientation chord. Check physical A/B numbering because the exact receiver's report has not been captured.
4. Unplug while holding a button: the GameBoy must release that button. Reinsert and re-pair to confirm hotplug. Verify SD loading/saving, I2C controller, touchscreen, display updates, and battery readings still work.

CI compiles the firmware and runs descriptor decoding and existing input regression tests. Passing CI does **not** constitute on-device USB or VBUS validation.
