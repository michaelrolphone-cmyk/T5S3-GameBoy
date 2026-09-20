#!/usr/bin/env python3
"""Build the complete original GameBoy source tree as a RiscRTE ET_DYN ELF.

Run after `pio run -e T5S3-GameBoy` so the exact Arduino/ESP-IDF compiler
flags and dependency discovery are available. No reduced/emulator-only source
list and no audio, display or storage stubs are permitted.
"""
import json
import os
from pathlib import Path
import re
import shlex
import shutil
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
missing = sorted(project - commands)
if missing:
    # Some sources may be deliberately unused by the standalone build; their
    # inclusion would introduce duplicate audio backends or peripheral code.
    print('Unused by standalone build:', *(str(p.relative_to(ROOT)) for p in missing))
if ROOT.joinpath('src/main.cpp').resolve() not in commands:
    raise SystemExit('Compilation database lacks original src/main.cpp')
if ROOT.joinpath('src/epd_video.cpp').resolve() not in commands:
    raise SystemExit('Compilation database lacks original src/epd_video.cpp')
if ROOT.joinpath('src/gbemu.c').resolve() not in commands:
    raise SystemExit('Compilation database lacks original src/gbemu.c')

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
    }.get(relative.as_posix(), source)
    obj = BUILD / 'objects' / relative.with_suffix(relative.suffix + '.o')
    obj.parent.mkdir(parents=True, exist_ok=True)
    tokens = []
    skip_next = False
    for index, token in enumerate(original_tokens):
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
        if token in (entry['file'], str(source)) or (
            (token.endswith('.cpp') or token.endswith('.c'))
            and Path(token).name == source.name
            and (Path(token).is_absolute() and Path(token).resolve() == source
                 or not Path(token).is_absolute() and (Path(entry['directory']) / token).resolve() == source)):
            continue
        tokens.append(token)
    tokens.extend([
        '-fPIC', '-mlongcalls', '-mtext-section-literals',
        '-fvisibility=hidden', '-ffunction-sections', '-fdata-sections',
        '-DPAPERBOY_RISCRTE_ELF=1', '-I' + str(ROOT / 'riscrte'),
        '-I' + str(SRC), '-I' + str(ROOT / 'src'),
        '-c', str(staged), '-o', str(obj),
    ])
    run(tokens)
    objects.append(obj)

if not objects:
    raise SystemExit('No application objects were compiled')
compiler = next(shlex.split(e.get('command', ''))[0] for e in entries
                if source_path(e) == ROOT.joinpath('src/main.cpp').resolve())
linker = compiler.replace('g++', 'g++').replace('-gcc', '-g++')
if not linker.endswith('g++'):
    linker = str(Path(compiler).parent / 'xtensa-esp32s3-elf-g++')
output = OUT / 'gameboy.elf'
run([linker, '-shared', '-nostdlib', '-nostartfiles', '-fPIC', '-mlongcalls',
     '-Wl,--hash-style=sysv', '-Wl,--gc-sections', *objects, '-o', output])
readelf = str(Path(linker).parent / 'xtensa-esp32s3-elf-readelf')
header = subprocess.check_output([readelf, '-h', str(output)], text=True)
if 'DYN (Shared object file)' not in header:
    raise SystemExit('Expected ET_DYN ELF module, not standalone firmware')
symbols = subprocess.check_output([readelf, '--dyn-syms', '--wide', str(output)], text=True)
for symbol in ('app_main', 'app_hardware_takeover'):
    if not any(re.search(r'\bGLOBAL\s+DEFAULT\s+\d+\s+' + symbol + r'\s*$', line)
               for line in symbols.splitlines()):
        raise SystemExit(f'Missing default-visible exported {symbol}')
undefined = sorted({f[7] for line in symbols.splitlines()
                    if len(f := line.split()) >= 8 and f[4] == 'GLOBAL' and f[6] == 'UND'})
(OUT / 'imports.json').write_text(json.dumps({'undefined': undefined}, indent=2) + '\n')
(OUT / 'gameboy.json').write_bytes((ROOT / 'riscrte/gameboy.json').read_bytes())
print(f'Built full-source ELF: {output}, objects={len(objects)}, imports={len(undefined)}')
if undefined:
    print('REQUIRES HOST EXPORT VERIFICATION:', ', '.join(undefined))
    # Do not mark the ELF installable until every import resolves in the merged
    # RiscRTE firmware. This intentional failure prevents a green but dead ELF.
    if os.environ.get('RISCRTE_EXPORT_LIST'):
        exports = set(Path(os.environ['RISCRTE_EXPORT_LIST']).read_text().splitlines())
        unresolved = sorted(set(undefined) - exports)
        if unresolved:
            raise SystemExit('Imports not exported by RiscRTE: ' + ', '.join(unresolved))
    else:
        raise SystemExit('No RISCRTE_EXPORT_LIST provided; refusing unvalidated hardware ELF')
