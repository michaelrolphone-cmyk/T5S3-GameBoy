# RiscRTE ELF conversion: behavior-preservation contract

This conversion starts at `master`. The original `src/main.cpp`, `src/paperboy_storage.cpp`, `src/paperboy_ui.cpp`, `src/gbemu.c`, and their existing support modules are the implementation and behavioral reference. **Port their functionality; do not replace the application with a reduced demonstration.** PR #5 is superseded and none of its artifacts are approved for deployment. This file records acceptance criteria, not a declaration that the port is finished.

## Must preserve

- The existing home/game/library/settings pages, ROM browser visual presentation, list paging and selection, all original touch and button behavior, game launch, switching and exiting. Do not auto-launch the first ROM.
- The existing ROM catalog semantics from `paperboy_storage_rescan()`: scan the SD root and one directory level, case-insensitive `.gb` and `.gbc` matching, sorted results, catalog capacity and file-size validation. `/Games` is one of the existing paths, not a new forced-only root. Preserve the built-in ROM fallback.
- Game frame timing, monochrome/dithering and redraw behavior to the extent the host's display API permits; retain game layout and overlays.
- The complete save semantics: cartridge RAM and RTC, existing canonical and legacy sidecar naming, memory quicksave, disk snapshots, save/restore actions, last-ROM preference and persistent configuration, including safe/atomic writes. Never silently discard a save or change its format.
- Original audio controls and engine behavior through an explicitly supported host audio capability, if available. A muted build is not feature parity and must not be represented as complete.
- Original battery, power, navigation and shutdown flows to the extent host-owned functionality can be delegated through public RiscRTE APIs. Identify and resolve any missing host capability rather than silently removing features.

## ELF/host requirements

- Inspect the actual headers and implementations for `T5AppApi`, `T5StorageApi`, and other needed public host APIs. Validate ABI version and struct size before calling optional members. Never guess at an ABI or copy an incomplete struct and assume it is complete.
- Use host storage paths rooted at `/sd` and verify the directory enumeration-to-read path contract. ROM reads must support whole cartridge images through the host's stream API when the single-call read API imposes limits; check exact byte count and close streams on all paths.
- Keep original source organization where possible, with ELF-only adapters for hardware access. Do not alter standalone firmware behavior. Any unavoidable capability gap is a documented blocker, not permission to delete the feature.
- Keep Xtensa ELF sections and every relocation compatible with RiscRTE's section loader (including `.text` mapping of interpreter code), verify imports and avoid unbound hardware symbols. Build and validate in CI with explicit failures on unsupported sections and relocation targets.

## Acceptance before marking ready

Check original-vs-ELF parity feature by feature, exercise ROM catalog selection with multiple ROMs including `.gbc` and nested first-level folders, load multi-bank ROMs with exact-read checks, verify save/restore compatibility and persistence, and confirm UI/control/audio/power behavior on hardware. Compiler success alone is not functional verification. Keep the replacement PR in draft until these criteria pass; do not merge without explicit user permission. No changes to the RiscRTE repository are authorized as part of this PR.
