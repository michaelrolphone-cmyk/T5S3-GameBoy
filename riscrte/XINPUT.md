# Xbox 360-format receiver support

Gameboy 1.2.18 can use RiscRTE's independently installed
`usb-xinput-gamepad` 0.1.1 driver for a `045e:028e` receiver. This receiver
advertises the Xbox 360 wired USB protocol even when its controller uses
a 2.4 GHz radio. The app requests `usb.xinput.gamepad@1` alongside its optional
HID gamepad and keyboard grants; either gamepad protocol can operate on its own.

Install the new RiscRTE driver and the updated Gameboy app. Keep the receiver
in its XInput mode. The driver discovers the vendor interface and input
endpoint, then publishes semantic gamepad events through the ordinary provider
API. The app does not acquire the PHY or start another USB host. Face buttons
follow Xbox names (A/B/X/Y); Back becomes Select. XInput D-pad and analog
directions combine, matching the hardware-tested standalone decoder. The driver
also recognizes compatible VID/PID clones, nonzero alternate settings and
`ff/5d/81` wireless-format packets; diagnostics select XInput by interface
protocol. Controller 0.1.11 adds interrupt endpoint stall recovery.

The test screen shows `XINPUT DRIVER UNAVAILABLE` if the receiver enumerates
but the driver is not installed/admitted, `XINPUT WAITING FOR REPORT` after
the interface is claimed, and `XINPUT GAMEPAD CONNECTED` after valid input.
`HID:0` is expected for the vendor interface. Connection events do not increase
the report counter. HID events cannot clear input held on the active XInput
controller, and disconnect/error events clear held buttons. The diagnostic scan uses the
existing exported string ABI; it does not depend on an unavailable `strstr`.

For a zero-device USB snapshot, install the paired controller 0.1.10 and host
0.1.3 diagnostics to show port attachment and the retained enumeration failure.
XInput decoding starts only after USB enumeration succeeds. Native regression
tests cover decoding and app delivery; physical enumeration and target ELFs
still require verification on the ESP32-S3 build/hardware.
