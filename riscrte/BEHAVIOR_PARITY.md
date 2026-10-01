# RiscRTE GameBoy ELF: preserve the original application

This port starts from the intact standalone `master` implementation. `src/main.cpp`, `src/epd_video.cpp`, `src/paperboy_ui.cpp`, `src/paperboy_storage.cpp`, the original emulator and their support modules are the implementation reference, not examples to re-create with reduced functionality. Closed PR #5 and its binaries are superseded. This contract does not claim an ELF is ready.

## Immediate compatibility approach

The standalone firmware retains the original raw `epd_video.cpp` and its hardware behavior. The RiscRTE ELF stages a `display.output` adapter in its place. Preserve the emulator, touch UI, audio, persistence, and game pacing; the installed display provider owns physical presentation.

The host performs an exclusive display handoff before entering GameBoy. The ELF acquires `display.output` on its owner task and borrows MONO1 frames from that provider; it does not stage the standalone panel/DMA implementation. Capability calls and release stay on the owner task, while the console worker writes pixels into its acquired frame. On exit the worker finishes before the provider lease is released and the host restores its UI. Shared I2C/PCA input access remains on the host owner task. This consumer conversion does not by itself prove full physical-driver extraction in the companion firmware.

The current candidate is GameBoy 1.3.13 with a minimum firmware version of 1.3.49 and the compatible `display-epd-video` 0.1.2 provider from Reader PR #220. The CI host checkout is pinned to Reader `95141e1bbefea7a5d0a2692d75b6aecd358a985c` from that companion branch; a firmware version number alone is not proof the capability exists. Separate save-confirmation PR #29 reserves version 1.3.12 and is not incorporated here. Reconcile release ordering and versions before publishing either candidate.

Host regression tests cover owner-task dispatch, a provider returning the wrong pixel format, rejected submissions retaining the frame, queued/active versus complete/superseded/failed presentations, and clean re-entry without a stale token. Unknown status remains pending, not fabricated success. The adapter's 24 Hz pacing counter is synthetic and is not physical scan-completion evidence.

## Behavioral invariants

- Keep the original game, ROM browser, settings, battery, SD and about screens, layout, navigation, touch mapping, controls, overlays, audio and gaming frame timing. Never silently load the first ROM.
- Keep the existing ROM scan (root plus first-level folders, including `/Games`), case-insensitive `.gb`/`.gbc`, sorted capped catalog, built-in fallback, error messages and ROM selection.
- Preserve cartridge RAM and RTC, canonical and legacy save paths and formats, last-ROM preference, configuration, memory quicksave, disk states and atomic writes.
- Preserve display output semantics through `display.output` acquisition, frame submission and owner-task release. The original `epd_video.cpp` remains the standalone implementation.
- Retain direct board interaction when essential, including touch, battery and audio. Avoid initializing shared SD or I2C hardware twice: use existing mounted VFS and shared-bus coordination where necessary, without changing the application's semantics.
- Ensure ROM reads use the correct VFS path and exact byte count, with chunked streaming for large files; do not confuse directory display names with absolute read paths. All opened resources close on failure and success.
- Build a valid Xtensa ET_DYN module, verify every imported symbol and relocation, and fail CI if a required ESP-IDF/Arduino symbol is unavailable. Allow practical host export additions for this compatibility app.

## Definition of ready

Produce a real ELF, not just a plan or an independently compiling test library. CI must build the original application sources into the ELF and test loader symbols/relocations plus ROM and save behavior. On device, verify launch, browser and multiple ROMs, high-speed display, audio, controls, save/load, clean exit and successful return to RiscRTE. Compilation is not hardware validation. The current display migration is PR #25; the owner controls merging and qualification.
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


### RiscRTE System ROM and save storage (1.3.2)

The RiscRTE ELF preserves legacy SD root/first-level ROM discovery and adds two
explicit application-state roots:

- `/System/State/Applications/gameboy/` for GameBoy-owned ROMs, battery saves
  and full emulator state snapshots;
- `/System/State/Applications/Rom Manager/` as an additional ROM source for
  the future Rom Manager app.

Both roots accept case-insensitive `.gb`/`.gbc` ROMs and one child-directory
level, while `.sav` and `.state` files are never added to the ROM catalog.
New RiscRTE battery saves and snapshots are always written to GameBoy's own
state directory using the ROM filename, even when the ROM itself was discovered
under Rom Manager. For example, a ROM at
`/System/State/Applications/Rom Manager/Tetris.gb` writes
`/System/State/Applications/gameboy/Tetris.gb.sav` and
`Tetris.gb.state`.

Existing adjacent `ROM.gb.sav`/`ROM.gb.state` and older
`ROM.sav`/`ROM.state` paths remain read-compatible so this storage change
does not strand previously created saves. The standalone firmware keeps its
existing SD-root/adjacent-sidecar behavior.

October 1 integration: the minimum firmware is 1.3.49 because Reader #220 adds the restricted display-provider backend binding absent from published 1.3.48. The GameBoy source, standalone display, save format and pacing remain unchanged. Publish save-confirmation #29 (1.3.12) before 1.3.13, or renumber that later candidate above any published 1.3.13. No unrelated save-confirmation changes are included.
