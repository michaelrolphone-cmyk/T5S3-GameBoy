# RiscRTE GameBoy native ELF

This directory builds a **separate Xtensa ESP32-S3 ET_DYN application** from the existing GameBoy CPU/PPU (`src/gbemu.c`, `src/crankboy_core/peanut_gb.h`), MiniGB software APU, and the original locally generated homebrew demo ROM. The original standalone PlatformIO firmware remains buildable. **Nothing in `michaelrolphone-cmyk/T5S3-Reader` is modified.** The RiscRTE checkout is only a pinned, read-only source of its public ELF ABI headers, export inventory, and matching Xtensa toolchain.

## Build and install

Install PlatformIO and download the cross toolchain by building the original firmware (or use the provided pull-request GitHub Actions build):

```sh
python -m pip install platformio==6.1.19
pio run -e T5S3-GameBoy
python riscrte/build_elf.py --sdk ../T5S3-Reader --output dist/gameboy.elf
```

The build script rejects non-Xtensa/non-ET_DYN output, absent `app_main`, undefined symbols not exported by the referenced RiscRTE firmware, and firmware-only IRAM sections. The CI workflow checks out RiscRTE SDK commit `fc1cb9210687a91cfde8a9e889b94e99b2c50d50`, builds the standalone firmware first to provision the compiler, builds `gameboy.elf`, verifies all staged checksums, and publishes both ELF and manifest in the PR artifact. Future approved tagged GitHub releases attach the **individual** `gameboy.elf` and `gameboy.json` alongside the standalone firmware binaries (no release ZIP required).

Copy both resulting files to the SD card (the names must match exactly):

```text
/Apps/gameboy.elf
/Apps/gameboy.json
/ROMs/example.gb           (optional, user-supplied compatible ROM)
```

Launch **GameBoy** from RiscRTE's Apps screen. Requires RiscRTE version **1.2.42 or newer** and its existing native ELF loader. The bundled locally generated homebrew demo is the first ROM selection, so no external game or commercial ROM is required. Scans `/ROMs`, `/roms`, and `/GameBoy/ROMs` on SD, up to 48 entries including the demo. Only DMG-compatible `.gb` or `.gbc` cartridges up to 4 MiB are supported; CGB-only games are rejected by the original emulator.

## Controls and saves

At the ROM library: Up/Down (or Left/Right) selects, Confirm launches, Back exits. During gameplay: Confirm=A, Back=B, directions=D-pad, Confirm+Right=Start, Back+Left=Select. The top touch strip offers Menu, Load State, and Save State. Two bottom touch strips offer the eight GameBoy buttons; touching one latches it momentarily. Hold Back+Confirm+Up for about a second to return to the ROM menu. Power/Home requests return to RiscRTE. The frontend disables automatic Back-as-exit for gameplay and restores it on return.

Cartridge battery RAM/RTC uses the existing PBSV save encoding in `<ROM>.sav`; quick states use `<ROM>.state`. The built-in demo uses `/GameBoy/builtin.gb.sav` and `/GameBoy/builtin.gb.state` if saved. Saves use RiscRTE's existing atomic file-write API; cartridge RAM is saved periodically and on exit. Keep the SD card inserted. The frontend streams cartridge loading in bounded chunks rather than importing direct SD filesystem drivers.

## Deliberate compatibility choices and limits

- **No RiscRTE modifications or new firmware symbols:** all host imports must pass actual firmware export validation, and the manifest enforces the firmware floor. The original emulator's ESP heap/time and IRAM source assumptions are replaced in this ELF build only.
- **Display:** existing RiscRTE app ABI has `clear`, `fill_rect`, `draw_text`, and `present`, but no framebuffer-blit member. Fast monochrome mode converts the original 160×144 GameBoy screen into 3×3 black pixel runs drawn as horizontal rectangles on the 540×960 e-paper panel; the host retains ownership of the panel. It aims for roughly 350 ms between display updates while the GameBoy CPU continues running. Expect slower output than the dedicated standalone firmware. No direct panel/SPI takeover is attempted.
- **Audio:** the software APU still runs and audio-related emulator state/save format remains intact, but its output adapter is deliberately muted: RiscRTE's ordinary app ABI does not expose the standalone firmware's PWM audio backend. It must not drive the board's LoRa reset/speaker GPIO as a shortcut.
- **Power/touch:** firmware retains its power, display, and physical input drivers. This is a native application and does not flash firmware, initialize hardware a second time, or alter RiscRTE source.
- **Validation:** CI checks ELF file type and exports/imports, checksum consistency and normal firmware build. A CI pass is not a claim of verified e-paper frame cadence, free heap on every board, speaker output, or hardware gameplay. These need actual device testing after the branch's ELF is installed.

Only use ROM images that you own or have permission to use. The workflow bundles no third-party game ROMs.
