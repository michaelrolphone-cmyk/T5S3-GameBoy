# Nintendo SNES/NES Classic Mini controller over I2C

This firmware supports the **SNES Classic Mini / NES Classic Mini I2C controller**, which uses the Wii-extension-style connector. A **1990s original SNES controller is not I2C** and cannot be connected directly. Use a suitable Wii-extension breakout or a correctly wired adapter; the controller's physical connector does not directly fit the T5S3 board header.

## Wiring

Connect the controller breakout to the T5S3 Pro's bottom I2C header, checking the actual connector orientation/pin labels before powering either device:

| SNES Mini breakout | T5S3 Pro header |
| --- | --- |
| SDA | GPIO39 / SDA |
| SCL | GPIO40 / SCL |
| VCC | **3.3 V only** |
| GND | GND |

**Do not use 5 V.** Do not assume a cable's conductor order or connect the legacy SNES plug to this header. The onboard PCA9535 is at `0x20`; the controller is at `0x52`. They share SDA/SCL, but each has its own slave address. The firmware already initializes the board `Wire` bus at 400 kHz and the driver reuses it without a second `Wire.begin()` or a whole-bus scan.

## Operation

- Connect the controller, open/play a Game Boy ROM, and use the physical D-pad, A/B, Start and Select. SNES X is an additional Game Boy A button; SNES Y is an additional Game Boy B button. L/R are ignored because the original Game Boy has no shoulder buttons.
- The touchscreen stays active: its buttons are ORed with the controller's button mask each emulated frame. The physical controller affects gameplay, not Settings navigation or power/shutdown.
- The driver probes only `0x52` (once per second while absent). On detection it sends `F0 55` and `FB 00` initialization, requests register `00`, reads six bytes, and decodes the active-low buttons in bytes 4 and 5. It polls no faster than every 16 ms and uses a 2 ms report settling interval.
- Short reads, invalid reports and unplugging release all buttons immediately and initiate a new probe after a second. Connection and disconnection are logged at 115200 baud with the `snes_mini` tag. No SD configuration or new library dependency is necessary.

Protocol and mapping reference: Albert Gonzalez, [Connecting a (S)NES Mini controller to an Arduino](https://albertgonzalez.coffee/projects/snes_mini_arduino/) and his [published SNES/NES Mini controller driver](https://github.com/theisolinearchip/nesmini_usb_adapter/blob/main/nesminicontrollerdrv.c).

## Hardware validation

1. With no controller connected, boot and verify touchscreen, RTC, battery status and display still work, with no continuous controller I2C traffic or log spam.
2. Attach the controller using the labeled 3.3 V/GND/SDA/SCL signals. Start a ROM and verify all four D-pad directions and A/B/Start/Select. Hold multiple buttons simultaneously. Test X/Y aliases if using an SNES Mini pad.
3. While holding a direction, unplug the controller. Confirm movement stops and touchscreen controls remain responsive; reattach and confirm automatic reconnection within about one second.
4. Verify periodic battery polling, EPD refresh and the existing PCA9535 power button continue to function while the controller is connected.

The PlatformIO build can validate compilation and artifact creation, but real controller communication and connector pinout require an on-device test.
