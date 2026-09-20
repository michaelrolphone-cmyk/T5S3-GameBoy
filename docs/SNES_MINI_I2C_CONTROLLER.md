# SNES/NES Classic Mini I2C controller

The supported controller is the SNES/NES **Classic Mini** Wii-extension style I2C gamepad, address `0x52`, **not** an original 1990s SNES serial controller. A correctly labeled Wii-extension breakout/adapter is required.

## Wiring (board and controller powered off)

| Controller breakout | T5S3 E-Paper Pro |
| --- | --- |
| SDA | GPIO39 / SDA |
| SCL | GPIO40 / SCL |
| VCC | **3.3 V, never 5 V** |
| GND | GND |

Check the *actual connector labels and orientation*, not conductor color or apparent pin order. Power off before changing wires. The controller and board share GPIO39/40 with the PCA9535 (`0x20`), touchscreen, RTC and battery devices. If the controller pulls SDA or SCL low, it can stop all those peripherals. Software cannot overcome a shorted, miswired or improperly powered shared bus.

## Operation and freeze protection

The controller is optional. It sends the `F0 55` and `FB 00` handshake to `0x52`, then selects register `00` and decodes six-byte reports, using the last two active-low bytes. SNES X/Y additionally map to Game Boy A/B; L/R have no Game Boy equivalent. Touchscreen and controller buttons are combined; menus and power remain touch-controlled.

Controller transactions now run in a low-priority FreeRTOS worker on the other core. The emulator reads only an atomic cached byte, without synchronously calling the controller's I2C or waiting through handshake delays. Failed reads immediately release buttons and retry after one second. Diagnostics are rate-limited and include transaction stage, return code, elapsed time and physical SDA39/SCL40 levels. The gamepad never scans the shared bus or reinitializes it. The driver uses `Wire.setTimeOut(15)` to bound shared I2C transactions; `Wire.setTimeout()` is an unrelated Stream timeout.

**The actual observed failure:** `Wire.cpp:499 i2cWriteReadNonStop returned Error 263` means `ESP_ERR_TIMEOUT` (`0x107`). The simultaneous touchscreen and BQ27220 errors show other devices on the *shared* bus also failing; on the SD-card page (`page=3`), the controller input path does not execute. Thus the failure is not established to be within the controller poller. A stuck/busy bus, incorrect wiring, inadequate pull-ups, power collapse or electrical contention must be ruled out before assuming software can fix it. EPD scan output can continue even when input and battery reads have timed out.

## Hardware fault isolation

1. Disconnect the controller and restart. Confirm touch and battery readings return to normal.
2. With power **off**, compare the physical I2C header pin labels to the controller breakout. Confirm 3.3 V/GND are not swapped, and SDA/SCL are not reversed; verify the controller's connector pins are indeed Wii-extension I2C, not a legacy SNES pad.
3. If possible, measure idle SDA and SCL: both should sit near 3.3 V. A line held near zero while the controller is connected indicates a bus fault. Confirm the 3.3-V rail also remains stable.
4. Only after wiring is verified, reconnect and review the rate-limited `snes_mini` serial diagnostics. `SDA39=0` or `SCL40=0` indicates the line is held low at the sample instant. If both are high but reads still time out, check the adapter pull-ups, connector, bus frequency and signal integrity.
5. Test gameplay buttons, hot-unplug release and reconnection while verifying touchscreen, power button, display and battery continue to work. Do not switch individual wires under power.

Protocol reference: Albert Gonzalez, [SNES Mini controller to Arduino](https://albertgonzalez.coffee/projects/snes_mini_arduino/) and his [driver](https://github.com/theisolinearchip/nesmini_usb_adapter/blob/main/nesminicontrollerdrv.c).

PlatformIO compilation is not a substitute for physical bus validation.
