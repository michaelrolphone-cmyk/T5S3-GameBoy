# Original application capability reconstruction

This working branch reconstructs the lost original-UI capability adapter. It is
not recovered commit e429db588040c19d6ca152066c36a32a564afa0a and is not a release.
The installed GameBoy 1.3.23 ELF remains the integration baseline until fresh
source, behavioral and target qualification succeeds.

All 57 original application/core/UI source files under `src/` and eight minimal
ABI support files exactly match the hashes in the recovered 1.3.23 build receipt.
They are present on published commit 164008d447724f42d027d5a59a844b8fdc7a545b.
`reconstruction/source-comparison.json` records the comparison; the full old
build receipt is retained beside it. Original source must remain byte-for-byte
unchanged. Runtime adaptation belongs in this directory and its staged output.

A later Drive recovery supplied original 1.3.22 adapter source (commit 9424f9e225a8351fa38e1956469221047999d03a), including all nine production files: backend.cpp, build.py,
geometry.hpp, include/Arduino.h, libc_compat.c, load_trace.hpp, platform.hpp,
stage.py and storage.cpp. The seven host-test files and twenty earlier validation records were also recovered. Comparison to the 1.3.23 receipt leaves five substantive changed files plus a version-only build change and two missing header-state validation records. Their behavior is being reconstructed and freshly qualified. Old logs cannot substitute for fresh verification. The provisional adapter rewrite is superseded by this recovered predecessor.

## Milestones

1. Preserve exact surviving source and its provenance on this working branch.
2. Reconstruct capability staging, storage, display/input lifecycle and the
   original ROM browser, scene, controls, settings and snapshot behavior.
3. Verify original UI/control mapping, ROM loading, save formats, interrupted
   flows and terminal ownership; build and audit the actual Xtensa ELF.
4. Apply display-independent logical pacing and latest-completed-frame
   coalescing, then compare complete CPU/APU/input histories with fast, 60 Hz,
   and 2300 ms displays and explicit CPU limits.
5. Publish the fresh source/proofs and compare manifest/ABI before integration.

The existing minimal frontend remains historical source. It is not the
replacement UI for this work. No panel waveform changes belong here.
