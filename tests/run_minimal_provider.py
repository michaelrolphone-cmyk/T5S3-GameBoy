#!/usr/bin/env python3
"""Verify GameBoy paths against the selected real X4 storage provider fixture."""
import argparse, os, subprocess, sys, tempfile
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser(description=__doc__)
for name in ('x4', 'runtime', 'reader'):
    p.add_argument('--' + name, type=Path, required=True)
p.add_argument('--sanitize', action='store_true')
a = p.parse_args()
with tempfile.TemporaryDirectory() as temp:
    build = Path(temp)
    subprocess.run([sys.executable, str(a.x4/'minimal/scripts/prepare_sdk.py'),
                    '--runtime', str(a.runtime), '--reader', str(a.reader),
                    '--output', str(build/'sdk')], check=True)
    flags = ['-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
             '-Wno-overflow', '-Wno-misleading-indentation', '-DX4_EXPECT_BATCHING=1',
             '-DX4_SD_TEST_SOURCE="' + str(a.x4.resolve()/'minimal/test/sd_test.c') + '"']
    if a.sanitize:
        flags += ['-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-omit-frame-pointer']
    includes = [build/'sdk', a.reader/'Drivers/storage_fatfs',
                a.reader/'Drivers/x4pro_board', a.reader, ROOT/'minimal',
                ROOT/'minimal/include', ROOT/'src']
    source = [ROOT/'tests/minimal_provider_path_test.c', ROOT/'minimal/model.c',
              ROOT/'minimal/touch.c', a.reader/'Drivers/storage_fatfs/fatfs/ff.c',
              a.reader/'Drivers/storage_fatfs/fatfs/ffunicode.c']
    binary = build/'provider-test'
    subprocess.run(['cc', *flags, *['-I'+str(d) for d in includes], *map(str, source), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=120,
                   env=dict(os.environ, ASAN_OPTIONS='detect_leaks=0', UBSAN_OPTIONS='halt_on_error=1'))
