# Standalone GameBoy: self-powered USB OTG HID gamepad and keyboard

The ESP32-S3 native USB host (GPIO19 D-, GPIO20 D+) accepts a standard HID gamepad or a full-size USB HID keyboard. Insert the ZAMPAM B0BWY5CJFR gamepad's 2.4 GHz USB receiver or a wired keyboard into an appropriate USB-C host adapter. The board supplies its own VBUS from the BQ25896. USB input is integrated directly into the standalone GameBoy firmware, not an RiscRTE ELF.

## Board-qualified VBUS configuration

The power implementation follows RiscRTE firmware v1.2.16 and the merged board-power fixes in [RiscRTE PR #101](https://github.com/michaelrolphone-cmyk/T5S3-Reader/pull/101) and [PR #102](https://github.com/michaelrolphone-cmyk/T5S3-Reader/pull/102). REG0A BOOSTV=9 and BOOST_LIM=2 produce **5.126 V with a 1.2 A PMIC hardware peak threshold** (cfg 0x92, or 0x9A retaining PFM). A 500 mA logical USB admission budget does not set the hardware boost threshold. The former 500 mA PMIC setting caused failures even with no attached device.

Before starting the USB host, the firmware checks for external input, checks historical and live BQ25896 faults separately, snapshots REG03/REG0A/REG02, enables OTG and allows 80 ms settling. It verifies the OTG status and at least 4.4 V measured VBUS inside 1.5 s, permits only a single *cleared* initial inrush within 250 ms followed by 200 ms clean operation, and fails closed on a new or persistent fault. Rollback and power-off verify sourcing has stopped before restoring charging or requesting battery cutoff. Regular battery service preserves a healthy active boost; low battery or unsafe faults stop boost without repeated restart attempts. Starts above 3.6 V and 15% SOC if reported; stops below 3.5 V or at/below 10% SOC. Own boosted VBUS is not displayed as an external charger.

**No externally powered USB hub is required by the design.** The physical USB-C source-role routing and BQ OTG-pin wiring must still be verified on the actual board. Do not inject 5 V externally, connect opposed supplies or attach VBUS to ESP32 GPIO. Application USB CDC is disabled (`ARDUINO_USB_MODE=0`, `ARDUINO_USB_CDC_ON_BOOT=0`) while the host owns the PHY; use BOOT/RESET ROM programming or separate debugging as needed.

## HID keyboard compatibility

Supports the USB HID **Keyboard/Keypad usage page (0x07), usages 0–255**, covering standard 104/108-key layouts including both modifiers, navigation cluster, F1–F12 and numeric keypad. Keys are identified by HID *usages*, not text, ASCII or the OS keyboard layout. Standard boot keyboards receive `SET_PROTOCOL(BOOT)` and use their 8-byte modifier/6-key reports. Report-protocol keyboards with an HID Keyboard application collection, a bounded 6KRO array or NKRO variable key bitmap, and optional Report ID are also decoded. An ErrorRollOver packet releases held keys instead of leaving them stuck. Short packets are rejected and unplug releases all keyboard/controller input.

The current USB host consumes one supported HID interface on **one USB device** at a time; using a keyboard and a separate USB gamepad simultaneously through a hub is not implemented. The original I2C SNES/NES Mini controller and touchscreen remain additive. Specialized vendor-only keyboards, consumer/multimedia usage-page shortcuts and boot-protocol-incompatible keyboards without a supported report descriptor are not supported. No typing/text editor or keyboard-driven ROM search is added; keyboard keys map to existing GameBoy controls.

### Logical keyboard bindings

| GameBoy/UI action | Keyboard keys |
|---|---|
| D-pad | Arrow keys, WASD or numeric keypad 8/4/2/6 |
| A / menu select | Z, J, Enter, numeric keypad Enter or keypad 5 |
| B / menu back | X, K, Escape, Backspace or keypad 0 |
| X = turbo A | C |
| Y = turbo B | V |
| L / R shoulder | Q / E |
| Select / Start | Tab / Space |
| Save | F5, Ctrl+S or keypad + |
| Load | F9, Ctrl+O or keypad - |
| Dim backlight | F6, Ctrl+- or keypad / |
| Brighten backlight | F7, Ctrl+= or keypad * |
| Open Settings | F1, F10 or Ctrl+Escape |
| Rotate orientation | F2, F11 or Ctrl+R |

Controller chords remain available with Q/E as shoulders and Tab/Space as Select/Start; the explicit function-key shortcuts are simpler. Keyboard commands trigger on the *press edge* (including modifier-combo activation), not on each USB report or frame. Ctrl+S suppresses the WASD Down mapping for that shortcut. Held directions and A/B remain level-triggered; C/V use existing turbo timing. Enter/Escape work on game-selection and settings menus because they map to the existing A/B navigation semantics.

## Gamepad compatibility

USB gamepad mode requires a standard HID Generic Desktop Game Pad/Joystick application collection, compatible interrupt IN endpoint and report descriptor (DirectInput/HID, not vendor-specific XInput/XUSB). Standalone default button usages: 1=B, 2=A, 3=Y, 4=X, 5=L, 6=R, 9=Select, 10=Start, with hat or X/Y D-pad. Start+R saves; Start+L loads; unmodified bumpers do nothing; Select+R brightens, Select+L dims; L+R+Start+Select opens Settings and L+R+Right rotates. A/B navigate menus, X/Y turbo in-game.

The RiscRTE ELF HID adapter uses the hardware-reported eight-button receiver
layout: B=0x01, A=0x02, Y=0x04, X=0x08, L=0x10, R=0x20, Start=0x40 and
Select=0x80. The first six mappings are unchanged; Start/Select are the corrected
modifier positions. The separate XInput provider has normalized Start=0x200 and
Select=0x100; 0x40/0x80 remain triggers there. Gamepad Test retains raw hexadecimal
readings and labels the modifiers for the active ELF input protocol.

## Acceptance checks

Flash a PR CI firmware artifact, boot from a charged battery without a charger and connect a keyboard through the host adapter. Verify `battery: USB VBUS source verified` and `usb_input: USB keyboard active interface=...`, then use arrow/WASD, Enter/Escape in Settings and the ROM list, Z/X in-game, C/V turbo, F5/F9 save/load, F6/F7 brightness and F1/F2 settings/rotation. Check both left/right Ctrl for shortcuts, keypad and the F10/F11 alternatives. Press/release several keys, unplug while holding Up, reinsert and confirm no stuck key or repeated action. Repeat with the gamepad receiver; verify SD, touchscreen, SNES Mini and battery management remain functional. If the keyboard does not enumerate, inspect VID/PID, candidate-interface and report-protocol diagnostics before assuming a power failure. A passing CI build/test does not measure VBUS or verify specific physical keyboard hardware.
