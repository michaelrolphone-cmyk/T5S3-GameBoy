# Original GameBoy loading diagnostics 1.3.21

This capability-only increment preserves the accepted 1.3.20 original UI, emulator, controls and ROM scan behavior. It records each selected-ROM attempt with a monotonically increasing ID and timestamps, separately from directory scanning. Named spans cover previous-cartridge persistence, file open and size, allocation, exact ROM reads, header/emulator initialization, save RAM/state helpers, configuration save, emulator start and the first completed playable display frame. Failures retain their stage and code; ordinary cleanup logs each released capability. After terminal custody refusal, no extra diagnostic/provider call is made.

All messages go through the existing bounded Runtime diagnostic callback. No synchronous filesystem writes were added. Each message is at most 239 text bytes; ROM read progress occurs once per 256 KiB, and playable-frame logs occur once per attempt. No ROM contents are logged. The startup identity and audio message now describe the capability target and silent audio backend.

Fifteen complete original-app scenarios pass normally and with ASan/UBSan, including file-open/read/header failures and a failed attempt followed by a successful second attempt. Assertions check stage order, timestamps, attempt IDs, exact 32-KiB/eight-call read totals, successful first completion, clean cleanup and no work after retention. LeakSanitizer is unavailable under executor ptrace; explicit allocation and custody checks stay enabled.

The separate production storage test links this adapter to the selected X4 SD transport and actual shared FatFs. Only the hardware/card wire is modeled. It reads 32 KiB, 256 KiB, 1 MiB and 4 MiB and compares every byte. Each workload asserts exactly ceil(bytes/4096) file-read calls, two file opens, no directory calls during ROM load, and zero sector writes. One MiB requires 2,065 sector reads and 43,542,590 scoped GPIO calls; four MiB requires 8,257 sectors and 174,107,102 calls. The one-ROM initial directory scan uses three sectors and no file-read or write calls. Host elapsed times are not device throughput measurements.

The associated independent Runtime 0.1.88 input uses the existing validated GPIO token hint table for reads. Its exact source is listed in qualification.json. That native optimization is not embedded in this ELF and needs deliberate product/native composition.

## Separately reproduced save/config defect

The production FatFs stat callback requires both output pointers. Existing GameBoy host_stat forwards optional null pointers, causing existing files to appear absent. The reproduction confirms an existing ROM is reported absent and an existing last-ROM config is silently treated as default. Therefore successful save-restore helper logs in this version mean only that the existing helper returned success; they do not prove that existing saved bytes were found or restored. This data-preservation repair is queued separately; nested directory feature work remains deferred.

Commands, exact source pins and log hashes are in qualification.json. No hardware result or product delivery is claimed by this checkpoint.
