#!/usr/bin/env python3
"""Link GameBoy CPU, PPU, APU, demo and RiscRTE frontend as a native ELF.

The SDK is a read-only checkout of T5S3-Reader. Nothing in RiscRTE is patched,
linked as a firmware binary, or changed by this build.
"""
import argparse
import hashlib
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
SOURCES = (
    'src/gbemu.c',
    'src/audio.c',
    'src/minigb_apu/minigb_apu.c',
    'riscrte/audio_output_mute.c',
    'riscrte/libc_compat.c',
    'riscrte/gameboy_app.c',
    'src/builtin_demo_rom.cpp',
    'riscrte/demo_bridge.cpp',
)


def run(args):
    print('+', ' '.join(map(str, args)), flush=True)
    return subprocess.check_output(list(map(str, args)), text=True,
                                   stderr=subprocess.STDOUT)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sdk', type=Path, default=Path(os.environ.get(
        'RISCRTE_SDK_PATH', '../T5S3-Reader')))
    parser.add_argument('--output', type=Path, default=ROOT / 'dist/gameboy.elf')
    parser.add_argument('--cc', default=os.environ.get('NATIVE_APP_CC'))
    args = parser.parse_args()
    sdk = args.sdk.resolve()
    if not (sdk / 'lib/NativeApps/include/T5AppApi.h').is_file() or not (
            sdk / 'scripts/native_app_symbols.py').is_file():
        parser.error('--sdk must identify a checked-out RiscRTE repository')
    sdk_scripts = str(sdk / 'scripts')
    sys.path.insert(0, sdk_scripts)
    from app_manifest import validate_manifest
    from native_app_symbols import firmware_exports, validate_imports

    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    manifest = validate_manifest(ROOT / 'riscrte/gameboy.c', output)
    # The ELF C source is gameboy_app.c; its published manifest is deliberately
    # gameboy.json so the sidecar matches the final gameboy.elf basename.
    compiler = args.cc or shutil.which('xtensa-esp32s3-elf-gcc')
    if not compiler:
        core = Path(os.environ.get('PLATFORMIO_CORE_DIR', Path.home() / '.platformio'))
        compiler = str(core / 'packages/toolchain-xtensa-esp32s3/bin/xtensa-esp32s3-elf-gcc')
    cc = Path(compiler)
    if not cc.is_file():
        resolved = shutil.which(str(compiler))
        if resolved:
            cc = Path(resolved)
        else:
            raise RuntimeError(f'Xtensa ESP32-S3 compiler missing: {compiler}')
    cxx = cc.with_name(cc.name.replace('-gcc', '-g++'))
    readelf = cc.with_name(cc.name.replace('-gcc', '-readelf'))
    for tool in (cc, cxx, readelf):
        if not tool.is_file():
            raise RuntimeError(f'Required ESP32-S3 toolchain component missing: {tool}')

    build = ROOT / '.pio/gameboy-elf'
    build.mkdir(parents=True, exist_ok=True)
    common = [
        '-Os', '-fPIC', '-mlongcalls', '-mtext-section-literals',
        '-fvisibility=hidden', '-ffunction-sections', '-fdata-sections',
        '-fno-asynchronous-unwind-tables', '-fno-unwind-tables',
        '-DPAPERBOY_RISCRTE=1', '-DGBEMU_FAST_MONO=1',
        '-D_POSIX_C_SOURCE=200809L',
        '-I' + str(ROOT / 'riscrte/include'),
        '-I' + str(sdk / 'lib/NativeApps/include'),
        '-I' + str(ROOT / 'src'),
    ]
    objects = []
    for name in SOURCES:
        source = ROOT / name
        if not source.is_file():
            raise RuntimeError(f'Missing original emulator or ELF source: {source}')
        obj = build / (name.replace('/', '__').rsplit('.', 1)[0] + '.o')
        is_cpp = name.endswith('.cpp')
        cmd = [cxx if is_cpp else cc, '-c', *common,
               '-std=gnu++17' if is_cpp else '-std=c11']
        if is_cpp:
            cmd.extend(('-fno-exceptions', '-fno-rtti'))
        cmd.extend((source, '-o', obj))
        print(run(cmd))
        objects.append(obj)
    print(run([cc, '-nostdlib', '-nostartfiles', '-shared', '-fPIC',
               '-Wl,--hash-style=sysv', '-Wl,--gc-sections',
               '-Wl,-Map,' + str(build / 'gameboy.map'),
               *objects, '-o', output]))

    header = output.read_bytes()[:32]
    if len(header) < 24 or header[:4] != b'\x7fELF' or header[4:6] != b'\x01\x01':
        raise RuntimeError('Not a 32-bit little-endian ELF binary')
    elf_type, machine = struct.unpack_from('<HH', header, 16)
    if elf_type != 3 or machine != 94:
        raise RuntimeError(f'Expected Xtensa ESP32-S3 ET_DYN, got type={elf_type}, machine={machine}')
    symbols = run([readelf, '--dyn-syms', '--wide', output])
    if not any(len(fields := line.split()) >= 8 and fields[3] == 'FUNC'
               and fields[4] == 'GLOBAL' and fields[6] != 'UND'
               and fields[7] == 'app_main'
               for line in symbols.splitlines()):
        raise RuntimeError('ELF does not export a defined app_main')
    imports = validate_imports(symbols, firmware_exports(sdk))
    if not {'t5_app_get_api', 't5_storage_get_api'} <= imports:
        raise RuntimeError('GameBoy ELF missing required RiscRTE host API imports')
    sections = run([readelf, '--section-headers', '--wide', output])
    if '.iram1.pgb' in sections or '.iram1.' in sections:
        raise RuntimeError('Firmware-only IRAM sections leaked into the dynamic ELF')
    if output.stat().st_size == 0:
        raise RuntimeError('Empty ELF')
    sidecar = output.with_suffix('.json')
    shutil.copyfile(manifest, sidecar)
    # Stage checksum agreement if this build follows the firmware staging step.
    sums = output.parent / 'SHA256SUMS'
    if sums.is_file():
        existing = [line for line in sums.read_text().splitlines()
                    if not line.endswith('  ' + output.name)
                    and not line.endswith('  ' + sidecar.name)]
        for path in (output, sidecar):
            existing.append(hashlib.sha256(path.read_bytes()).hexdigest() +
                            '  ' + path.name)
        sums.write_text('\n'.join(existing) + '\n')
    print(f'Validated {output} ({output.stat().st_size} bytes), manifest {sidecar}')
    print('Firmware-resolvable imports:', ', '.join(sorted(imports)))


if __name__ == '__main__':
    main()
