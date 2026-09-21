# USB OTG HID gamepad support (standalone GameBoy)

The standalone firmware uses the ESP32-S3 native USB PHY (GPIO19 D-, GPIO20 D+) as a host for the ZAMPAM B0BWY5CJFR controller's 2.4 GHz **USB receiver**. It is not RiscRTE, ELF, Bluetooth or the SNES Mini I2C jack. The firmware discovers HID interfaces and parses report descriptors without assuming an undocumented receiver VID/PID.

## On-board USB receiver power

**The T5S3 now requests its own 5 V VBUS using its BQ25896 OTG boost regulator; an externally powered USB hub is not the intended operating configuration.** `battery_begin()` configures the charger and gauge, verifies that the battery is above 3.6 V (and above 15% if the gauge is available), confirms no external VBUS, disables charging, programs 5 V output and the 500 mA boost current limit, then sets OTG_CONFIG. It waits for converter startup and verifies the charger reports OTG mode before the console loop starts USB enumeration. `battery_service()` preserves OTG instead of restoring charging every 30 seconds, checks for faults and low battery while boosting, and shuts the boost off and latches a fault rather than repeatedly restarting a shorted receiver. Power-off disables boost before testing for *external* USB power or requesting BATFET shutdown. The battery indicator no longer counts the device's own 5 V boost as an incoming charger.

Use a suitable **USB-C OTG adapter or USB-A receiver adapter** with the controller receiver. Do not connect the board to a PC/charger on that connector while using the receiver, inject external 5 V, connect two VBUS sources, or connect VBUS to ESP32 GPIO. When incoming VBUS is detected, the firmware does not start its own boost. A passive adapter's USB-C role/CC wiring and the board's physical OTG-pin/connector routing still require verification: software cannot create source-role CC resistors or bridge missing hardware connections. In particular, TI's BQ25896 requires its physical OTG pin HIGH as well as OTG_CONFIG=1; the firmware reports a verification failure if the converter does not enter OTG mode. If it fails, inspect the board schematic and receiver connection rather than bypassing the protection.

Battery protection: boost starts above 3.6 V and 15% SOC when reported, stops below 3.5 V or at/below 10% SOC, and refuses to start if both the fuel gauge and charger ADC are unreadable. Boost or thermistor faults cause shutdown and a latched no-retry state until restart. Charging behavior remains the original profile when host boost is inactive. The 500 mA limit is intentional for a low-power HID dongle, not a general-purpose high-current USB output.

The build uses `ARDUINO_USB_MODE=0` and `ARDUINO_USB_CDC_ON_BOOT=0` to free the native PHY. The application cannot simultaneously expose USB CDC serial on the same connector while hosting the receiver. ROM bootloader recovery/flash remains possible with the board's BOOT/RESET download procedure; a separate serial debug connection may be required for logs.

## Receiver compatibility

A USB interface must expose standard HID with a Generic Desktop Game Pad or Joystick application collection, interrupt IN endpoint, and hat or X/Y fields. Firmware fetches the descriptor and validates report IDs and lengths. Intended for **DirectInput/HID** mode. Proprietary XInput/XUSB, keyboard-only and vendor-specific modes are not supported. Exact USB identity and physical button layout of this receiver are not yet hardware-confirmed.

Default HID usage mapping: Button 1=B, 2=A, 3=Y, 4=X, 5=L, 6=R, 9=Select, 10=Start. Hat or X/Y supplies the D-pad. Modify `src/usb_hid_report.cpp` if the receiver enumerates with a different physical button mapping.

## Controls

USB input combines with touchscreen and the existing I2C controller. D-pad/A/B play; X repeatedly presses A (turbo); Y repeatedly presses B. R saves, L loads, Select+R brightens, Select+L dims, L+R+Start+Select opens Settings, and L+R+Right rotates. Menus use D-pad to navigate, A to select and B to go back. Removing the receiver releases any held buttons and reinsertion can re-enumerate. The GameBoy consumes one HID gamepad interface at a time; multiplexed multi-player receivers are not implemented.

## Hardware acceptance test

1. Flash the firmware artifact from the PR's PlatformIO CI workflow. Run from a sufficiently charged battery with **no USB charger connected**. Insert the controller's receiver using the correct USB OTG adapter. Pair and select the receiver's HID/DirectInput mode if selectable.
2. Confirm diagnostic `battery: USB VBUS boost active: 5 V / 500 mA limit` followed by `usb_gamepad: receiver enumerated address=`, VID/PID and `HID gamepad active interface=`. If boost fails, inspect `USB VBUS boost verification failed mode=... reg03=... fault=...`; an I2C write succeeding does not mean 5 V physically reached the connector. Measure VBUS-to-GND at the connector with appropriate test equipment if the receiver stays dark; never probe or supply power through GPIO.
3. Confirm D-pad/A/B, menu selection, X/Y turbo, L/R shortcuts, Settings/rotation, unplug/replug and continued SD/touch/I2C functionality.
4. Verify that 30-second charger maintenance does **not** turn boost off, that a low-battery shutdown turns it off, that an overcurrent/boost fault never triggers an automatic retry loop, and that power-off doesn't falsely interpret its own boost as external USB charging. Confirm normal battery-charging behavior with the receiver removed and the board rebooted into charger-connected operation.

CI builds firmware and runs HID-report and controller regression tests. CI **cannot** confirm the board's USB-C source-role wiring, OTG pin state, actual 5 V, receiver-specific HID mapping, or fault response under physical loads; these remain hardware acceptance checks.
