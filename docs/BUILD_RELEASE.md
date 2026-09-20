# Building and releasing T5S3-GameBoy

The only supported PlatformIO target in this repository is `T5S3-GameBoy`, for the LilyGO T5S3-4.7 E-Paper Pro. The build uses the custom `boards/T5-ePaper-S3.json` and the version in `[version]` of `platformio.ini`.

## Download a build without publishing a release

On GitHub, open **Actions → GameBoy PlatformIO Build → Run workflow** and select `master`. Pushes to `master` and pull requests also build automatically. Open the successful run and download its `gameboy-t5s3-<commit>` artifact. No tag or GitHub Release is created by this workflow.

The artifact contains:

| Asset | Use |
| --- | --- |
| `T5S3-GameBoy-vVERSION-app.bin` | Application image only. Flash at **0x10000** onto a device with a compatible bootloader/partition layout. Do not flash at 0x0. |
| `T5S3-GameBoy-vVERSION-merged.bin` | Complete initial-install/USB image, including bootloader, partition table and app. Flash at **0x0**. Do not use as an app-only/OTA image. |
| `T5S3-GameBoy-vVERSION.elf` | Debug symbols only; do not flash. |
| `SHA256SUMS` | SHA-256 hashes of all three outputs. |

The staging script fails the job if any file is absent or empty, if the ESP32 image headers are invalid, if the version is not embedded in the app, or if the merged image does not contain exactly that application at 0x10000. The merged image is made from PlatformIO's actual upload images and offsets, not an independently guessed partition layout.

## Cut a GitHub Release

1. Set `[version]` in `platformio.ini` to the intended version, merge it to `master`, and ensure its build passes.
2. Open **Actions → Cut GameBoy release → Run workflow**, select `master`, and enter the matching version tag (for example, `v1.2.0`).
3. The workflow validates the version, rebuilds and checks the binaries, uploads a workflow artifact, creates the tag, then publishes a GitHub Release with the four files above. It will not silently overwrite an existing release or reuse a tag that points to another commit.

Alternatively, edit `.github/release-request.json` on `master` with `"enabled": true` and a matching `"tag"`. Pushing that file triggers the same release workflow. It is initially disabled to prevent a release merely from merging the workflow. Subsequent releases require a new version in both `platformio.ini` and the request file. Releases do not commit generated binary files back into source control.

## Local build and flashing

```sh
python -m pip install platformio==6.1.19
pio run -e T5S3-GameBoy
python scripts/stage_firmware.py
(cd dist && sha256sum -c SHA256SUMS)
```

For a clean installation with a USB serial port, replace the port and version below with your own:

```sh
python -m pip install esptool
python -m esptool --chip esp32s3 --port YOUR_PORT write_flash 0x0 dist/T5S3-GameBoy-v1.2.0-merged.bin
```

Only for an existing compatible installation, application-only flashing uses the *app* image at address `0x10000`. Never interchange these images or offsets. For ordinary direct PlatformIO programming, `pio run -e T5S3-GameBoy -t upload --upload-port YOUR_PORT` remains supported.

The build ships the built-in demo ROM; an untracked `src/rom/test_rom.h` from a local ROM conversion is not present on GitHub Actions runners. Release workflows do not download or bundle external game ROMs.
