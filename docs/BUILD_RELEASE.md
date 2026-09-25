# Building and releasing T5S3-GameBoy

The repository produces two deliverables from the same GameBoy source tree:

- standalone firmware for the LilyGO T5S3-4.7 E-Paper Pro; and
- an installable RiscRTE ELF app with its manifest.

The standalone PlatformIO target is `T5S3-GameBoy` and uses the custom `boards/T5-ePaper-S3.json`. Its release version comes from `[version]` in `platformio.ini`. Every published RiscRTE app uses that same version: the release workflow binds `riscrte/gameboy.json` to the validated firmware release version and checks the generated sidecar before publishing.

## Download a development build

Open **Actions → GameBoy PlatformIO Build → Run workflow** and select `master`. Pushes to `master` and pull requests also build automatically. Download the `gameboy-t5s3-<commit>` artifact from the successful run. This is a GitHub Actions artifact; GitHub automatically downloads it as a ZIP. It does not create a tag or GitHub Release.

The standalone build contains:

| File | Use |
| --- | --- |
| `T5S3-GameBoy-vVERSION-app.bin` | Application image only; flash at **0x10000** on a compatible installation. Never flash at 0x0. |
| `T5S3-GameBoy-vVERSION-merged.bin` | Complete USB image including bootloader, partition table, and app; flash at **0x0**. Not an app-only/OTA image. |
| `T5S3-GameBoy-vVERSION.elf` | Standalone debug symbols; do not flash. |
| `SHA256SUMS` | SHA-256 hashes for the standalone outputs. |

The staging script rejects missing/empty files, invalid ESP32 headers, a missing embedded version, and merged images that do not contain the exact app at 0x10000. The merged image uses PlatformIO's actual upload images and offsets.

The **Build faithful GameBoy RiscRTE ELF** workflow independently rebuilds the full source tree with the current RiscRTE toolchain, verifies all imported symbols against the current RiscRTE firmware export inventory, rejects direct `usb_host_*` imports, validates the ELF with RiscRTE's loader validator, and uploads `gameboy.elf`, `gameboy.json`, and `imports.json` as a CI artifact.

## Cut a release

1. Update `[version]` in `platformio.ini` to the intended standalone release version and merge the change to `master`.
2. Make sure the standalone PlatformIO, RiscRTE ELF, and parity checks are green on the merge commit.
3. Tag that commit and push the tag, for example `git checkout master && git pull && git tag v1.2.4 && git push origin v1.2.4`. Push a Git tag rather than publishing an empty GitHub Release yourself; the workflow creates the Release and attaches the binaries.
4. **Cut GameBoy release** verifies that the tag matches the standalone embedded version and points to a commit on `master`. It first builds and preserves the standalone firmware, then checks out current RiscRTE `master`, matches its toolchain, builds and validates the installable ELF, and only then creates the GitHub Release.

The Release publishes these individual assets:

| Asset | Purpose |
| --- | --- |
| `T5S3-GameBoy-vVERSION-app.bin` | Standalone application image for offset **0x10000**. |
| `T5S3-GameBoy-vVERSION-merged.bin` | Complete standalone flash image for offset **0x0**. |
| `T5S3-GameBoy-vVERSION.elf` | Standalone firmware debug symbols; not flashable. |
| `gameboy.elf` | Installable RiscRTE GameBoy application. |
| `gameboy.json` | RiscRTE manifest containing app compatibility, version, ELF size, and SHA-256. |
| `SHA256SUMS` | Integrity hashes covering all published binaries plus the RiscRTE manifest. |

The versioned standalone `.elf` and `gameboy.elf` are different artifacts: the former is debug output for the standalone firmware, while the latter is the dynamically loadable RiscRTE application.

You can also use **Actions → Cut GameBoy release → Run workflow** on `master` with a matching tag, or set `.github/release-request.json` to `"enabled": true` with the matching `"tag"` and push it on `master`. These routes create the tag if needed and publish the same individual files. Publishing refuses to overwrite an existing release or reuse a tag pointing at another commit, and generated binaries are never committed back into source control.

## Local standalone build and flashing

```sh
python -m pip install platformio==6.1.19
pio run -e T5S3-GameBoy
python scripts/stage_firmware.py
(cd dist && sha256sum -c SHA256SUMS)
```

For a clean installation over USB serial, replace the port and version below with your own:

```sh
python -m pip install esptool
python -m esptool --chip esp32s3 --port YOUR_PORT write_flash 0x0 dist/T5S3-GameBoy-v1.2.4-merged.bin
```

Only for an existing compatible installation, application-only flashing uses the *app* binary at address `0x10000`. Never interchange these images or offsets. Direct PlatformIO programming remains supported with `pio run -e T5S3-GameBoy -t upload --upload-port YOUR_PORT`.

The GitHub build ships the built-in demo ROM; an untracked local `src/rom/test_rom.h` is absent on GitHub Actions runners. Release workflows do not download or bundle external game ROMs.
