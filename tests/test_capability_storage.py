#!/usr/bin/env python3
"""Build the real storage boundary with rom_port and a fault-injected volume."""
import argparse
import os
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument('--sdk', type=Path, required=True)
parser.add_argument('--sanitize', action='store_true')
parser.add_argument('--build', type=Path, default=ROOT / 'build/capability-storage-test')
args = parser.parse_args()
args.build.mkdir(parents=True, exist_ok=True)
flags = ['-Wall', '-Wextra', '-Werror', '-g', '-O1']
if args.sanitize:
    flags += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-pie', '-no-pie']
includes = [f'-I{ROOT / name}' for name in ('capability', 'src', 'riscrte')] + [f'-I{args.sdk}']
obj = args.build / 'rom_port.o'
exe = args.build / 'storage-test'
subprocess.run(['cc', '-std=c11', *flags, *includes, '-c', str(ROOT / 'riscrte/rom_port.c'), '-o', str(obj)], check=True)
subprocess.run(['c++', '-std=c++11', *flags, *includes, str(ROOT / 'capability/storage.cpp'),
                str(ROOT / 'tests/capability_storage_test.cpp'), str(obj), '-o', str(exe)], check=True)
env = dict(os.environ, ASAN_OPTIONS='detect_leaks=0:abort_on_error=1', UBSAN_OPTIONS='halt_on_error=1')
scenarios = ['catalog', 'truncation', 'media', 'scan_error', 'dir_close', 'read', 'read_close',
             'read_error', 'read_count', 'alloc', 'config', 'recover', 'empty', 'write',
             'write_error', 'write_count', 'sync', 'write_close', 'verify', 'rename_old',
             'rollback', 'rollback_hold', 'backup_remove', 'yield_hold']
cases = [(s,) for s in scenarios] + [('abi', str(i)) for i in range(5)]
atomic_calls = None
for case in cases:
    result = subprocess.run([str(exe), *case], text=True, capture_output=True, env=env)
    if result.returncode:
        raise RuntimeError(f'{case}: {result.stdout}{result.stderr}')
    print(result.stdout, end='')
    match = re.search(r'atomic_calls=(\d+)', result.stdout)
    if match:
        atomic_calls = int(match[1])
assert atomic_calls is not None
# Inject terminal retention at every provider call in a complete atomic update.
for position in range(1, atomic_calls + 1):
    result = subprocess.run([str(exe), 'terminal', str(position)], text=True, capture_output=True, env=env)
    if result.returncode:
        raise RuntimeError(f'terminal {position}: {result.stdout}{result.stderr}')
    assert 'held=1' in result.stdout, result.stdout
print(f'PASS all {len(cases)} scenarios and {atomic_calls} terminal provider boundaries' + (' with ASan/UBSan' if args.sanitize else ''))
