# Original GameBoy UI, independent display timing (1.3.24)

This working branch is a source-qualified reconstruction, not recovered commit
`e429db588040c19d6ca152066c36a32a564afa0a` and not a hardware-qualified release.
Saved product binaries are comparison evidence only. Build all new products from
this source; do not substitute the previously installed 1.3.23 ELF.

## Source provenance

All 57 original application/core/UI files under `src/` and eight minimal ABI
support files exactly match the recovered 1.3.23 build receipt. The builder checks
all original `src/` hashes before staging. The original ROM browser, GameBoy
frame, controls, settings, snapshot format and CPU/APU implementation remain.

The complete original 1.3.22 capability adapter was recovered from a Drive source
archive, commit `9424f9e225a8351fa38e1956469221047999d03a`, tree
`e2139fc92ee60220bd43c8dc52532d3749c16eac`. Of the 36 capability paths in the
1.3.23 receipt, 28 match exactly; six differ and two validation records are absent.
The six are backend.cpp, build.py (version only), stage.py, storage.cpp,
tests/lifecycle.cpp and tests/run_lifecycle.py. This branch reconstructs the
header LOAD boundary and adds independent timing to that recovered predecessor.
`reconstruction/` records all comparisons. The provisional handwritten adapter
was superseded by the recovered predecessor. Historical validation logs are
retained for provenance only; current qualification is separate.

## Timing and frame custody

The original GameBoy pacer advances CPU/APU/audio and input irrespective of
physical display readiness. Completed gameplay images retain the original render
stride. Full scene composition and scaling are performed only when presentation
is available. A bounded two-slot scene mailbox coalesces visual changes and
preserves an independent writable baseline. Only the newest complete scene is
submitted; provider-owned pixels remain immutable until terminal status.

`epd_video_can_submit()` is a nonblocking readiness probe and
`cap_service_display()` services pending visual intent. Normal flips never drain
or wait. Exit discards unsubmitted visuals and drains only already-submitted
custody before releasing memory/grants. Lost or retained custody stops subsequent
CPU, allocator and provider work. These semantics parallel shared app-local
frame_ready/frame_drain; this separate adapter imports neither the portable
adapter nor a new Runtime/provider ABI. No panel waveforms are changed.

Header LOAD refreshes snapshot availability, tries the original memory snapshot
first, then the existing disk fallback. Original atomic save/rename/sync and
sidecar recovery semantics are retained. Missing/corrupt/read-failed snapshots
remain controlled failures. Every retained boundary is checked before further
emulator or allocator activity.

## Fresh qualification

- 33 staged original-app journeys pass normally and under ASan/UBSan, including
  ROM failures/retry, header save/load faults, retained custody, exit and mailbox.
- Identical scripted 10-second inputs at 1 ms, 17 ms and 2300 ms display latency
  produce 597 equal logical/audio steps, complete CPU/APU/input/time histories,
  and final snapshots. The slow display submits five frames; its four gameplay
  images are the newest completed frames 138, 276, 414 and 552.
- Injected 30 ms CPU work reduces both fast and slow cases to 323 equal steps.
  These virtual-time tests establish display independence, not physical FPS.
- Normal and sanitized histories/final snapshots are byte-identical. Test-only
  normalization removes named pointer fields, preserving scalar CPU/APU state,
  RAM and VRAM. Production snapshot format is unchanged.
- 20 obsolete scenes coalesce without waiting or additional emulation; dirty rows
  merge, newest pixels win and exit discards queued visuals.
- UI references: 18 renders, 71 original controls, 8 controller checks, 3,240,000
  scaled source pixels, 216,000 border pixels; both physical panel orientations.
- Storage: 29 fault cases plus 96 terminal provider boundaries in both modes.
- Actual Runtime app-phase lifecycle and native 800x480 journeys pass.
- Fresh Xtensa ELF passes production loader validation and relocation audit;
  manifest capabilities and dynamic imports match 1.3.23 (version changes only).

Physical X4 throughput and audible output are not established here. The recovered
capability audio backend is intentionally silent; logical APU/audio service is
preserved. Hardware acceptance remains required.

## Rebuild

Use Python 3 and the Xtensa ESP32-S3 GCC 8.4 toolchain. Run `capability/build.py`
with `--runtime`, `--system`, `--sdk`, `--cc` and a fresh `--output` directory.
The sealed external build receipt records exact dependency hashes, staged source,
compiler, imports and ELF audit. System timezone sources and current append-only
SDK headers are required; no saved ELF is an input.

Reproduce host qualification with `capability/tests/run_lifecycle.py` using those
Runtime/System/SDK sources, once normally and once with `--sanitize`; use
`--real-runtime` for the actual Runtime phase fixture. UI reference and storage
runners remain in `capability/tests/` and `tests/` respectively.
