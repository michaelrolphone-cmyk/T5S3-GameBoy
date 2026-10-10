# Existing save/config stat contract repair

The shared FatFs provider requires both size and directory output pointers. The GameBoy adapter previously forwarded nulls when a caller needed only existence or one field. This made existing files appear absent, including last-ROM configuration and save data.

The sole production behavior change supplies two local outputs on every valid stat call, checks the admitted successful return, then copies only fields the original caller requested. No discovery algorithm, search location, directory traversal or UI changes are included.

The actual selected X4 SD transport and shared FatFs reproduce the failure before this repair. Afterward the same test detects an existing ROM and reads the existing last-ROM config. It overwrites and rereads config, then atomically writes, discovers, reads and replaces a 51,340-byte saved-state blob, comparing every byte. Normal and ASan/UBSan executions pass. The fake volume now enforces the same required-pointer contract; all 29 storage cases and all 96 terminal provider boundaries still pass in both modes. Fifteen original-app lifecycle cases pass with ASan/UBSan.

The earlier 1.3.21 target and its known-defect evidence remain frozen. Nested ROM discovery remains deferred. LeakSanitizer is unavailable under executor ptrace; allocation/custody assertions and ASan/UBSan remain active. No physical device result is claimed.
