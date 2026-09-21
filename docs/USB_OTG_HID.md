# Standalone GameBoy: self-powered USB OTG HID gamepad

This branch runs the ESP32-S3 native USB host (GPIO19 D-, GPIO20 D+) and powers the ZAMPAM B0BWY5CJFR controller's 2.4 GHz USB receiver from the onboard BQ25896. No external powered hub is intended. USB HID and battery management run directly in the standalone GameBoy firmware; this is not an RiscRTE ELF.

## Board-qualified VBUS power configuration

GameBoy's BQ25896 host-power sequence is based on the known-good RiscRTE firmware v1.2.16 and the subsequent board-power driver fixes in RiscRTE PRs [#101](https://github.com/michaelrolphone-cmyk/T5S3-Reader/pull/101) and [#102](https://github.com/michaelrolphone-cmyk/T5S3-Reader/pull/102):

- REG0A BOOSTV=9, BOOST_LIM=2: **5.126 V output, 1.2 A PMIC peak-current limit** (configuration `0x92`, or `0x9A` if preserving the existing PFM bit). The 500 mA USB **logical admission budget** is separate and must not be written into BOOST_LIM. The earlier migration's 500 mA PMIC setting produced boost faults even with no attached device.
- Preflight checks external input / power-good / VBUS input detection and reads REG0C **twice**: first historical fault, then live fault. A historical cleared fault does not block startup; a live fault does.
- Snapshot REG03, REG0A and REG02. Set BOOST/ADC/OTG, then wait **80 ms** to settle. Within a **1.5 s** startup deadline, require REG03 OTG enabled, REG0B VBUS_STAT=OTG and REG11 ADC indicating at least **4.4 V**. The initial ADC can lag approximately 1 second.
- Accept at most **one cleared startup inrush fault** in the first 250 ms, only if OTG remains asserted and the next 200 ms are continuously clean. Persistent, repeated or late faults, failed I2C/ADC, low battery, or external VBUS conflicts fail closed. The initial 500 mA configuration must not be used as a workaround.
- On failure or power-off, turn OTG and charging off, verify the source is off, restore the snapshot/charger profile and verify readback before releasing source ownership. Uncertain shutdown remains latched and blocks BATFET cutoff. The regular charger service never overrides a live USB host source and checks battery/faults at one-second intervals.

Battery gating starts above 3.6 V and above 15% SOC when reported; host boost stops below 3.5 V or at/below 10% SOC. When the gauge is unavailable, the charger's battery ADC is used; unreadable voltage blocks startup. Own OTG VBUS is not reported as an incoming charger. Fault-related boost shutdown is not automatically retried.

Use a USB-C host/OTG adapter appropriate for the board, insert the controller's USB **receiver** and pair it. Never inject another 5 V source, bridge VBUS to GPIO or connect the PC/charger and the receiver to opposed supplies. Software can enable BQ boost but cannot repair a missing OTG-pin or USB-C CC/source-role circuit; this physical path still requires board verification.

## HID compatibility and controls

The receiver must enumerate as a standard HID Generic Desktop Game Pad or Joystick with an interrupt IN endpoint and compatible report descriptor. The firmware reads the descriptor; DirectInput/HID is supported, proprietary XInput/XUSB and keyboard-only modes are not. The exact receiver HID identity and physical mapping have not been verified. Default usage mapping is Button 1=B, 2=A, 3=Y, 4=X, 5=L, 6=R, 9=Select, 10=Start, with hat or X/Y for D-pad. Only one HID gamepad interface is used.

D-pad/A/B play; X pulses A (turbo), Y pulses B; R saves, L loads, Select+R brightens, Select+L dims, L+R+Start+Select opens Settings, L+R+Right rotates. Menus: D-pad navigate, A select, B back. USB inputs coexist with touchscreen and I2C SNES controller. Removal releases held keys; reconnection enumerates again.

Native USB application CDC is disabled (`ARDUINO_USB_MODE=0`, `ARDUINO_USB_CDC_ON_BOOT=0`) while host owns the PHY. Use separate debugging or BOOT/RESET ROM programming if needed.

## Hardware acceptance

Flash a CI-built PR binary and boot from a charged battery without a USB charger. Check the `battery` logs for `USB VBUS source verified` and `USB VBUS boost active: 5126 mV, 1200 mA PMIC peak`; a verified `cfg=0x92` is expected unless PFM is preserved. Look for `usb_gamepad: receiver enumerated address=`, VID/PID, and `HID gamepad active interface=`. If boost fails, inspect the raw `p/s/v/prev/now/bat/cfg/conv/ms` snapshot; confirm the USB connector's VBUS with appropriate equipment before blaming HID. Check input mapping, hotplug, SD, touchscreen, I2C and remaining battery functions. Check fault shutdown/no retry, boost persistence during battery service and proper VBUS-off on power-off.

CI verifies compilation and host-side HID/controller tests, **not electrical VBUS behavior or this receiver's actual reports**.
