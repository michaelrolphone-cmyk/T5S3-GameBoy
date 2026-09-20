# RiscRTE ELF port

This directory builds the GameBoy emulator as a RiscRTE native application. It is deliberately a compatibility port, not a reference implementation of RiscRTE hardware/capability architecture, and it does not require changes to the RiscRTE firmware repository.

## Build

Install PlatformIO so the ESP32-S3 Xtensa toolchain is present, then run:

```sh
pio pkg install -e T5S3-GameBoy
python riscrte/build_elf.py
```

Outputs:

- `dist/riscrte/gameboy.elf`
- `dist/riscrte/gameboy.json`

Copy both into RiscRTE's Apps installation area (or install them through the current package/app workflow if supported by the firmware build).

## ROMs

The ELF scans `/sd/ROMs` and loads the first regular file ending in `.gb` up to 4 MiB. ROM data is not bundled in the ELF. Only use ROM images you are authorized to use.

## Controls

- RiscRTE Left/Right/Up/Down -> GameBoy D-pad
- Confirm -> A
- Tap bottom-left -> B
- Tap bottom-middle -> Select
- Tap bottom-right -> Start
- RiscRTE app-exit gesture exits the emulator

## Port boundary

The standalone firmware's board initialization, charger, direct EPD driver, SD driver, touch driver, RTC I2C and PWM audio backend are not linked into the ELF. The ELF reuses the existing `gbemu.c`/Peanut-GB core, uses RiscRTE's app/storage compatibility APIs for presentation, input and ROM access, and provides small local heap/timer/audio compatibility shims. Audio is currently muted.

The e-paper framebuffer is translated to black horizontal runs using the existing RiscRTE `fill_rect()` app API. That prioritizes compatibility without a RiscRTE firmware change; hardware testing should determine whether a later optimized framebuffer/blit path is needed.
