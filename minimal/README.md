# GameBoy for the headless Runtime

This separate capability-only frontend runs the repository's complete CrankBoy
`gbemu.c` and MiniGB APU on RiscRTE 0.1.77 or a later compatible table. The legacy
standalone T5S3 firmware and Reader ELF builders remain independent.

It supports DMG and DMG-compatible `.gb`/`.gbc` files, read from the selected SD
volume or handed off through `file.open`. CGB-only games are rejected by the
existing core. The APU still advances and preserves its internal state, but the
output is deliberately silent. This frontend does not yet expose cartridge
saves, save states, touch controls or audio output. It performs no SD writes.
The GameBoy cartridge header checksum policy remains the original core policy.

The MONO1 frontend accepts a bounded surface at least 480×432 in landscape.
On X4's 480×800 raw surface it renders an 800×480 landscape layout, with a
centered 480×432 game view. Core white bits are inverted to the canonical
MONO1 black-bit convention. Fast mode requests low-latency presentation; Paper
mode requests quality presentation. The picker uses the selected mode too. The physical driver owns the waveform.
While a frame is pending, emulation and navigation continue; the app never
modifies a submitted surface or submits another until completion.

Controls:

- Picker: arrows select; Confirm opens; Back goes to the parent folder; Home
  opens the configured launcher. Page Back changes Fast/Paper; Page Forward
  advances ten entries.
- Game: arrows are the D-pad; Confirm is A; Back is B; Page Back/Forward are
  Select/Start. Hold Back for 900 ms to return to the ROM picker. Home leaves
  directly for the configured launcher, even when launched by a file browser.
- USB/BLE controllers participate through the deployment's navigation provider.
  This app requests no raw USB, radio or touch hardware capability.

Version 1.3.16 fixes the broker/volume path boundary: the UI and `file.open`
use `/sd/...`, while storage-volume calls receive `/...` (root `/sd` becomes
`/`). The selected X4 FatFs provider has no implicit `sd` subdirectory.
Cleanup now waits for an owned pending display even after an unrelated input
failure; an actual provider failure or timeout still retains ownership safely.

Default-visible `GAMEBOY t_ms=... stage=... result=...` diagnostics name
capability acquisition/release, storage refresh/open/close, ROM loading and
initialization, input startup, the first frame and presentation transitions,
failures and cleanup. The bounded Runtime diagnostic callback is used directly;
there are no app file writes or per-frame logs. Retention logs its named failure
before requesting invocation retention. The original .38 log alone does not
prove which provider caused its retained invocation; the next device run can
identify the exact operation. The separate 1.3.15 multi-touch work is preserved.

The catalogue holds at most 96 entries and scans at most 512 entries or two
seconds per folder, with scheduler checkpoints. ROM reads use 4096-byte chunks,
a 4 MiB limit and a 15-second deadline. Back cancels loading. Incomplete reads
never initialize the core. Presentation has a five-second deadline. Uncertain
storage close, display completion or grant release retains the invocation;
it cannot continue I/O, free provider-owned resources or launch another app.

## Build and tests

Use the official Xtensa ESP32-S3 GCC 8.4.0 toolchain and the selected Runtime
checkout. No Arduino framework is needed for the application ELF.
The dedicated minimal-port CI pins Runtime
`b2d9dc1b90498665c95b70ec3bffa61245f346ec` ([Runtime PR56](https://github.com/michaelrolphone-cmyk/RiscRTE/pull/56)).
Its source tree exactly matches the locally qualified `.77` source
`3093683cd505e15eb3be595ea0403a6e2c82eecb`. This pin includes the append-only
configured-default callback needed by Home; a version number alone is not
a sufficient compatibility check.

```sh
NATIVE_APP_CC=/path/to/xtensa-esp32s3-elf-gcc \
  python3 minimal/build.py --runtime ../RiscRTE
python3 tests/run_minimal.py --runtime ../RiscRTE
ASAN_OPTIONS=detect_leaks=0 python3 tests/run_minimal.py --runtime ../RiscRTE --sanitize
```

The builder adapts only the core's allocator, monotonic clock and ELF section
annotations, plus odd-address emulated reads/writes. The latter use byte access
instead of unaligned native words, a defect reproduced by the real test ROM
under UBSan. Fixed-step position-independent integer helpers avoid GCC8's
non-PIC division archive. The original emulator/APU files remain untouched;
every staging substitution asserts the expected original sites.

The output includes the ELF, strict app manifest, source/compiler/hash receipt
and notices. Both the production ELF validator and packed-section/relocation
audit run on the actual Xtensa output. Host tests execute the real emulator/APU
against an original synthetic ROM, exercise both orientations, validate native
integer arithmetic and cover malformed inputs, cancellation and retention.
They do not execute Xtensa instructions or qualify physical refresh behavior.

## Product integration

Add `gameboy.elf` and `gameboy.json` to the immutable installed store and declare
exact grants for `display.output@1`, `input.navigation@1`, `storage.volume@1`
and global `file.open@1`. The X4 .26 provider graph uses instances 3, 6, 9 and 0
respectively; a new product must verify its actual graph. Add the app to the
launcher catalogue and rebuild the native/store binding and admission receipt.
Do not copy this ELF into a frozen old product or claim a generic `.77` stock
BIN is a qualified X4 native composition. Product integration follows the X4
owner's power-fix priority.

This is reconstructed source. The earlier unpublished `dcdbc5499...` port and
its 122,472-byte ELF were lost. This build has a new source ID and byte identity;
it is not a recovered copy of that artifact.
