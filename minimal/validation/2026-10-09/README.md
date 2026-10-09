# X4 GameBoy source and validation receipts

These receipts preserve the exact completed local builds. Their `source` pins are not rewritten to the GitHub publication commits. `source-map.json` maps each original commit to its public, tree-identical counterpart. Both 1.3.15 and 1.3.16 descend independently from public `16242c55437134400e8ce4db696530c022bb8575`; 1.3.17 retains both parents in that order. This receipt-only commit adds this directory without changing application source.

## Builds

- 1.3.15: simultaneous touch, 38 lifecycle scenarios plus contact/model fixtures; 87,588-byte ELF, SHA256 `be3b94b1ffcc2582c73174fb335af997075c529e4b947262abef6f14b254a1ff`.
- 1.3.16: immediate four-grant diagnostic build; root-relative SD paths, Fast picker, pending-display cleanup and named logs. 25 lifecycle scenarios; 83,916-byte ELF, SHA256 `8cc2174ccbcc21efc03d8355f41c5b18afa3654efa2e4b7267d8b09d0b778d4f`.
- 1.3.17: combines both lines; 39 lifecycle scenarios, contact/model fixtures, and direct GameBoy-path integration with actual X4 SD/shared FatFs. 97,588-byte ELF, SHA256 `f478892cc71959d9945af9a1f4a4c27c3f7270041a724b925fb82ff45430cb20`.

All host suites passed normally and with ASan/UBSan. All three GCC8 target ELFs passed production structural validation and packed-section/relative-relocation auditing (451, 544 and 627 relocations respectively). No commercial ROM was used. The actual binaries remain identified by their existing source/hash receipts; this publication does not create or relabel a device image or release.

## Reproduce

Run from the corresponding source checkout, with the selected Runtime checkout and official Xtensa ESP32-S3 GCC8.4.0 compiler:

```sh
python3 tests/run_minimal.py --runtime ../RiscRTE
ASAN_OPTIONS=detect_leaks=0 python3 tests/run_minimal.py --runtime ../RiscRTE --sanitize
NATIVE_APP_CC=/path/to/xtensa-esp32s3-elf-gcc python3 minimal/build.py --runtime ../RiscRTE
```

The 1.3.15 build used Runtime `b2d9dc1b90498665c95b70ec3bffa61245f346ec`; 1.3.16 and 1.3.17 used delivered Runtime `faa8f62936a5889c65cce3f271758d9d95f21d7b`. The 1.3.17 production-volume test uses the selected X4 .38 checkout plus Reader `86241815411a925b6f246ff8437a33dce3d20af6`:

```sh
python3 tests/run_minimal_provider.py --x4 ../Xteink-X4 --runtime ../RiscRTE --reader ../T5S3-Reader
python3 tests/run_minimal_provider.py --x4 ../Xteink-X4 --runtime ../RiscRTE --reader ../T5S3-Reader --sanitize
```

The selected later Reader source includes the SD operation-budget and export-prepare contracts required by the X4 fixture. Its historical baseline `aac8c06...` alone does not provide that complete test composition.

## Receipt integrity and limits

The JSON files are unchanged originals. Logs are losslessly gzip-compressed; `source-map.json` records compressed and original byte sizes and SHA256 hashes, plus each Git blob SHA. Decompress with `gzip -dc FILE.log.gz` to recover the exact original log. Normal and sanitized logs can be byte-identical because these deterministic fixtures print the same results.

`root-contract-before.log.gz` records the original app failing the strict root-relative contract. `cleanup-negative-control.log.gz` restores only the old cleanup `&& !failed` guard and reproduces unwanted retention after an input failure. `production-root-contract.log.gz` confirms the real SD/FatFs provider rejects broker-prefixed paths; the 1.3.17 provider logs also validate GameBoy's actual conversion, 32 KiB of exact synthetic bytes and clean teardown.

The original hardware log does not establish which provider caused its retained invocation. The named diagnostics are intended to identify that operation on the next device run. Physical panel refresh, simultaneous hardware contacts and held-report cadence remain unverified. The immediate 1.3.16 build and previously qualified 1.3.14 binary are unchanged.
