# T060 save-state reliability / 1.3.25

Baseline: shipped 1.3.24, a579dedd2a76956c7dcd654434cff9a556beb37c. Original src/UI/controller files remain byte-identical; the build retains their sealed hash check. Native runtime and ROM discovery/read algorithm remain unchanged.

Reproduced: failed SD save publishes the new in-memory quicksave before the atomic disk write succeeds; subsequent LOAD selects that failed save over the previous durable snapshot. Capture into a separate candidate and publish memory only when the snapshot commit succeeds. Tests inject write, sync and rename failures and assert previous disk bytes remain exact and LOAD uses them.

Reproduced: a current-format snapshot with invalid zero-ROM-bank offset is accepted, constructs an out-of-range pointer, then crashes on emulation. Validate current-layout serialized cartridge geometry and bank offsets before the legacy loader mutates the live emulator. Valid snapshot format is unchanged; legacy upgrade paths remain supported. This fixes the reproduced malformed-state path; no physical user snapshot was available to identify their exact crash trigger. This is not a complete arbitrary-state corruption validator.

Fresh-emulator load-last regression checks restored normalized CPU/RAM/APU state equals the durable snapshot, disk bytes are untouched, and emulation continues. New operation timing stages cover capture, decode, atomic save, open, write, sync, close, readback verification and rename commit. Existing read sizing/allocation/read/restore and ROM timing remain. No per-frame storage logging or scanning changes.

38 lifecycle journeys pass normally and under Linux ASan/UBSan. Deterministic 1ms/17ms/2300ms display cases retain 597 equal simulation/audio steps and identical final states; compute-delay cases retain 323 equal steps. These are host measures, not physical loading/FPS claims. Original UI and storage fault suites are qualified separately before release. Hardware testing remains outstanding.
