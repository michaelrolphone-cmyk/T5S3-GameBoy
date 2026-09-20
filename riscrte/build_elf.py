#!/usr/bin/env python3
"""Build the complete original GameBoy source tree as a RiscRTE ET_DYN ELF.

Run after `pio run -e T5S3-GameBoy` so the exact Arduino/ESP-IDF compiler
flags and dependency discovery are available. No reduced/emulator-only source
list and no audio, display or storage stubs are permitted.
"""
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'riscrte'))
from prepare_elf import stage
from stage_core import stage_core

OUT = ROOT / 'dist/riscrte'
BUILD = ROOT / 'build/riscrte'
SRC = BUILD / 'src'
OUT.mkdir(parents=True, exist_ok=True)
SRC.mkdir(parents=True, exist_ok=True)
stage(SRC)
stage_core(SRC)
HOST_ROOT = Path(os.environ.get('RISCRTE_HOST_ROOT', ROOT / '_riscrte')).resolve()
HOST_INCLUDE = HOST_ROOT / 'lib/NativeApps/include'
if not HOST_INCLUDE.joinpath('T5StorageApi.h').exists():
    raise SystemExit(f'RiscRTE native API headers not found under {HOST_INCLUDE}')


def run(cmd):
    print(' '.join(shlex.quote(str(a)) for a in cmd), flush=True)
    subprocess.run(list(map(str, cmd)), cwd=ROOT, check=True)


def source_path(entry):
    p = Path(entry['file'])
    return (p if p.is_absolute() else Path(entry['directory']) / p).resolve()


def original_sources():
    files = set(ROOT.glob('src/**/*.c')) | set(ROOT.glob('src/**/*.cpp'))
    files |= set(ROOT.glob('lib/bq*/src/**/*.c')) | set(ROOT.glob('lib/bq*/src/**/*.cpp'))
    return {p.resolve() for p in files}


compiledb = ROOT / 'compile_commands.json'
if not compiledb.exists():
    run(['pio', 'run', '-e', 'T5S3-GameBoy', '-t', 'compiledb'])
if not compiledb.exists():
    raise SystemExit('PlatformIO did not create compile_commands.json')
entries = json.loads(compiledb.read_text())
project = original_sources()
commands = {source_path(e): e for e in entries if source_path(e) in project}
missing = sorted(project - set(commands))
if missing:
    # Some sources may be deliberately unused by the standalone build; their
    # inclusion would introduce duplicate audio backends or peripheral code.
    print('Unused by standalone build:', *(str(p.relative_to(ROOT)) for p in missing))
for required in ('src/main.cpp', 'src/epd_video.cpp', 'src/gbemu.c',
                 'src/paperboy_storage.cpp', 'src/paperboy_ui.cpp', 'src/audio.c'):
    if ROOT.joinpath(required).resolve() not in commands:
        raise SystemExit(f'Compilation database lacks original {required}')

# Build the already regression-tested host catalog/stream adapter with the
# exact C flags used by the original emulator core.
commands[ROOT.joinpath('riscrte/rom_port.c').resolve()] = commands[
    ROOT.joinpath('src/gbemu.c').resolve()]

objects = []
for source, entry in sorted(commands.items()):
    original_tokens = shlex.split(entry.get('command', '')) if entry.get('command') else list(entry['arguments'])
    if not original_tokens:
        raise SystemExit(f'No compiler command for {source}')
    relative = source.relative_to(ROOT)
    staged = {
        'src/main.cpp': SRC / 'main.cpp',
        'src/epd_video.cpp': SRC / 'epd_video.cpp',
        'src/gbemu.c': SRC / 'gbemu.c',
        'src/paperboy_storage.cpp': ROOT / 'riscrte/paperboy_storage_host.cpp',
    }.get(relative.as_posix(), source)
    obj = BUILD / 'objects' / relative.with_suffix(relative.suffix + '.o')
    obj.parent.mkdir(parents=True, exist_ok=True)
    tokens = []
    skip_next = False
    for token in original_tokens:
        if skip_next:
            skip_next = False
            continue
        if token in ('-o', '-MF', '-MT', '-MQ'):
            skip_next = True
            continue
        if token.startswith(('-o', '-MF', '-MT', '-MQ')) and len(token) > 3:
            continue
        if token in ('-MMD', '-MD', '-MP', '-c'):
            continue
        entry_source = source_path(entry)
        if token in (entry['file'], str(source), str(entry_source)) or (
            (token.endswith('.cpp') or token.endswith('.c'))
            and (Path(token).is_absolute() and Path(token).resolve() == entry_source
                 or not Path(token).is_absolute() and
                    (Path(entry['directory']) / token).resolve() == entry_source)):
            continue
        tokens.append(token)
    tokens.extend([
        '-fPIC', '-mlongcalls', '-mtext-section-literals',
        '-fvisibility=hidden', '-ffunction-sections', '-fdata-sections',
        '-DPAPERBOY_RISCRTE_ELF=1', '-I' + str(ROOT / 'riscrte'),
        '-I' + str(SRC), '-I' + str(ROOT / 'src'), '-I' + str(HOST_INCLUDE),
        '-c', str(staged), '-o', str(obj),
    ])
    run(tokens)
    objects.append(obj)

if not objects:
    raise SystemExit('No application objects were compiled')
compiler = next((shlex.split(e['command'])[0] if e.get('command') else e['arguments'][0])
                for e in entries if source_path(e) == ROOT.joinpath('src/main.cpp').resolve())
linker = compiler.replace('-gcc', '-g++')
if not linker.endswith('g++'):
    linker = str(Path(compiler).parent / 'xtensa-esp32s3-elf-g++')
output = OUT / 'gameboy.elf'
# The firmware loader maps only canonical sections by name. Pack IRAM_ATTR
# routines, constructors and linker-created tables there at ELF link time;
# don't require a special firmware loader just to run this application.
layout = ROOT / 'riscrte/elf_loader_layout.ld'
run([linker, '-shared', '-nostdlib', '-nostartfiles', '-fPIC', '-mlongcalls',
     '-Wl,--hash-style=sysv', '-Wl,--gc-sections', '-Wl,-T,' + str(layout),
     *objects, '-o', output])
readelf = str(Path(linker).parent / 'xtensa-esp32s3-elf-readelf')
header = subprocess.check_output([readelf, '-h', str(output)], text=True)
if 'DYN (Shared object file)' not in header:
    raise SystemExit('Expected ET_DYN ELF module, not standalone firmware')
# Symbols alone are insufficient: reject any output the firmware would fail
# to map before publishing a manifest, package, or GitHub artifact.
run([sys.executable, ROOT / 'riscrte/audit_elf_layout.py', output])
symbols = subprocess.check_output([readelf, '--dyn-syms', '--wide', str(output)], text=True)
for symbol in ('app_main', 'app_hardware_takeover', 'app_module_init', 'app_module_fini'):
    if not any(re.search(r'\bGLOBAL\s+DEFAULT\s+\d+\s+' + symbol + r'\s*$', line)
               for line in symbols.splitlines()):
        raise SystemExit(f'Missing default-visible exported {symbol}')
undefined = sorted({f[7] for line in symbols.splitlines()
                    if len(f := line.split()) >= 8 and f[4] == 'GLOBAL' and f[6] == 'UND'})
(OUT / 'imports.json').write_text(json.dumps({'undefined': undefined}, indent=2) + '\n')

# The source manifest is a template, not an installable release sidecar. Bind
# the actual output ELF's length and digest after linking, never a stale
# firmware image, prior artifact, or manually maintained checksum.
payload = output.read_bytes()
if not 52 <= len(payload) <= 8 * 1024 * 1024:
    raise SystemExit(f'GameBoy ELF is outside the RiscRTE 8 MiB app limit: {len(payload)} bytes')
metadata = json.loads((ROOT / 'riscrte/gameboy.json').read_text())
if metadata.get('file_name') != output.name:
    raise SystemExit('GameBoy manifest filename does not match the built ELF')
metadata['size_bytes'] = len(payload)
metadata['sha256'] = hashlib.sha256(payload).hexdigest()
(OUT / 'gameboy.json').write_text(json.dumps(metadata, indent=2) + '\n')
print(f'Built full-source ELF: {output}, objects={len(objects)}, imports={len(undefined)}, size={len(payload)}')
if undefined:
    print('REQUIRES HOST EXPORT VERIFICATION:', ', '.join(undefined))
    if os.environ.get('RISCRTE_EXPORT_LIST'):
        exports = set(Path(os.environ['RISCRTE_EXPORT_LIST']).read_text().splitlines())
        unresolved = sorted(set(undefined) - exports)
        if unresolved:
            raise SystemExit('Imports not exported by RiscRTE: ' + ', '.join(unresolved))
    else:
        raise SystemExit('No RISCRTE_EXPORT_LIST provided; refusing unvalidated hardware ELF')
