# RiscRTE GameBoy ELF: preserve the original application

This port starts from the intact standalone `master` implementation. `src/main.cpp`, `src/epd_video.cpp`, `src/paperboy_ui.cpp`, `src/paperboy_storage.cpp`, the original emulator and their support modules are the implementation reference, not examples to re-create with reduced functionality. Closed PR #5 and its binaries are superseded. This contract does not claim an ELF is ready.

## Immediate compatibility approach

GameBoy is a privileged, board-specific ELF. It **may directly operate the T5S3 hardware** and use ESP-IDF, Arduino and board-specific APIs. Compliance with the future portable-app/provider architecture is not a prerequisite. Do not replace `epd_video.cpp` with `T5AppApi` rectangles, replace the original touch UI with a text menu, mute audio, drop persistence, or alter game pacing just because the native-app facade lacks a service. Do not build a thin firmware-hosted proxy for the GameBoy display driver. Compile and retain the original hardware implementation in the GameBoy ELF, making minimal, guarded ELF-specific changes only where required for linking, initialization and orderly teardown. Standalone firmware behavior must remain unchanged.

The short-term host exception is an **exclusive whole-display handoff**: after validating GameBoy's package and before `app_main`, RiscRTE stops rendering and releases its display backend, bus/DMA and shared panel ownership. GameBoy starts its own original display pipeline, independently drives the display for the duration of the app and stops its tasks/interrupts/DMA, frees panel handles and returns ownership before its ELF is unloaded. RiscRTE then reinitializes its backend and redraws the prior UI even if GameBoy initialization fails. Do not unload a module while tasks, callbacks or DMA still execute its code. A minimal compatibility change in the RiscRTE host is authorized for this temporary takeover; do not turn it into a full capability/driver-system migration or make it a general architectural prerequisite. Protect shared I2C/power peripherals from simultaneous ownership.

## Behavioral invariants

- Keep the original game, ROM browser, settings, battery, SD and about screens, layout, navigation, touch mapping, controls, overlays, audio and gaming frame timing. Never silently load the first ROM.
- Keep the existing ROM scan (root plus first-level folders, including `/Games`), case-insensitive `.gb`/`.gbc`, sorted capped catalog, built-in fallback, error messages and ROM selection.
- Preserve cartridge RAM and RTC, canonical and legacy save paths and formats, last-ROM preference, configuration, memory quicksave, disk states and atomic writes.
- Preserve physical display performance by retaining the original `epd_video.cpp` raw scan, waveform, dirty-row, double-buffer and DMA implementation in the ELF.
- Retain direct board interaction when essential, including touch, battery and audio. Avoid initializing shared SD or I2C hardware twice: use existing mounted VFS and shared-bus coordination where necessary, without changing the application's semantics.
- Ensure ROM reads use the correct VFS path and exact byte count, with chunked streaming for large files; do not confuse directory display names with absolute read paths. All opened resources close on failure and success.
- Build a valid Xtensa ET_DYN module, verify every imported symbol and relocation, and fail CI if a required ESP-IDF/Arduino symbol is unavailable. Allow practical host export additions for this compatibility app.

## Definition of ready

Produce a real ELF, not just a plan or an independently compiling test library. CI must build the original application sources into the ELF and test loader symbols/relocations plus ROM and save behavior. On device, verify launch, browser and multiple ROMs, high-speed display, audio, controls, save/load, clean exit and successful return to RiscRTE. Compilation is not hardware validation. Keep PR #7 draft and unmerged until tested; the owner controls merging.
### Controller rendering and shortcuts (1.2.22)

External controller and keyboard buttons feed the emulator without animating the
on-screen touch controls. Only touch button changes invalidate their highlights;
gamepad presses and turbo pulses retain the game-only dirty region and do not
produce per-button INFO logs. Page, battery, notice and orientation updates still
refresh the full scene when needed.

Save is Start+R and load is Start+L; bumpers alone have no action. Select+L/R
adjusts brightness, and Start+Select+L+R opens Settings. Either the modifier or
bumper may complete a two-button chord; held chords do not repeat. Start/Select
are consumed through release after a shortcut. The full Settings chord wins and
its release cannot generate shoulder actions. Hold L+R before adding Start and
Select to assemble Settings without first invoking a two-button shortcut.

Native regression coverage executes the staged input/render blocks for 1,200
controller frames with zero full-screen redraws, retains touch feedback, and
checks HID/XInput modifier combinations and every Settings release order.
Physical frame rate and receiver behavior still require hardware confirmation.


### Receiver button mapping (1.2.25)

Hardware testing confirmed the controller performance improvement. The receiver's
corrected Gamepad Test readings identify the ELF HID button mapping:

| Button | Raw hexadecimal mask |
| --- | --- |
| A | 0x01 |
| B | 0x02 |
| X (turbo A) | 0x04 |
| Y (turbo B) | 0x08 |
| L | 0x10 |
| R | 0x20 |
| Select | 0x40 |
| Start | 0x80 |

The ELF HID backend uses these eight-button receiver masks. Gamepad Test keeps
showing the raw bits and labels 0x40/0x80 as Select/Start. The XInput provider
continues using its normalized Start=0x200, Select=0x100 mapping; its 0x40/0x80
trigger bits are not modifier aliases. Standalone USB mappings are unchanged.

The regression test feeds the reported masks through provider acquisition,
owner polling, snapshots and console sampling. It checks individual modifiers,
save/load, brightness, Settings, rotation, A/B navigation and X/Y turbo. The 1.2.24 code fails
on the first reported Start press. Updated hardware shortcut confirmation is pending.

### Controller full redraw (1.2.26)

In the ELF, Start+Select with neither bumper held requests the same full panel
refresh as the hardware BOOT button: white, black, white, then reconstruction of
the current page into both display buffers. It works on every page and resets
the frame pacer after the clear. Ordinary controller input retains game-only
rendering.

The redraw fires once when the second modifier is pressed. Holding the chord
or releasing its buttons in either order cannot repeat it or trigger save/load
or brightness actions. Start+Select are consumed through release. The complete
Settings chord has priority and can still complete after a redraw; hold L+R
first to open Settings without a preliminary redraw.

Regression tests cover HID and XInput press/release orders, redraw-to-Settings
transitions, and the actual staged hardware/controller refresh code. The latter
verifies the clear sequence, both panel buffers, frame-pacer reset, gameplay/menu
routing and a single clear for simultaneous BOOT/controller requests.
