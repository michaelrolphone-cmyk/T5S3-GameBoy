#!/usr/bin/env python3
"""Render unmodified original UI with minimal host stubs; no backend integration."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
from PIL import Image, ImageDraw

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser()
parser.add_argument('--output', type=Path, default=Path(__file__).parent / 'render-output')
args = parser.parse_args()
out = args.output.resolve()
out.mkdir(parents=True, exist_ok=True)
stub = out / 'include'
stub.mkdir(exist_ok=True)
(stub / 'Arduino.h').write_text('''#pragma once
#include <stdint.h>
uint32_t millis();
struct ReferenceEsp {
  uint32_t getFlashChipSize() const { return 16U * 1024U * 1024U; }
  uint32_t getPsramSize() const { return 8U * 1024U * 1024U; }
};
static const ReferenceEsp ESP{};
''')
sources = [root / 'src' / name for name in (
    'paperboy_ui.cpp', 'mono_canvas.cpp', 'paperboy_landscape.cpp',
    'paperboy_controller_navigation.cpp', 'paperboy_orientation.cpp', 'paperboy_game_clock.cpp')]
identity_inputs = [*sources,root / 'capability' / 'geometry.hpp']
source_hashes = {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest() for p in identity_inputs}
command = ['g++', '-std=c++17', '-g', '-Wall', '-Wextra', '-Werror',
           '-I' + str(stub), '-I' + str(root / 'src'),
           str(Path(__file__).parent / 'render_reference.cpp'), *map(str, sources)]
for mode, flags in [('normal', ['-O2']),
                    ('sanitized', ['-O1', '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-no-pie'])]:
    folder = out / mode
    folder.mkdir(exist_ok=True)
    binary = folder / 'render-reference'
    build = [*command, *flags, '-o', str(binary)]
    build_result = subprocess.run(build, capture_output=True)
    (folder / 'build.log').write_bytes(build_result.stdout + build_result.stderr)
    build_result.check_returncode()
    result = subprocess.run([str(binary), str(folder)], capture_output=True,
                            env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0:halt_on_error=1',
                                 'UBSAN_OPTIONS': 'halt_on_error=1:print_stacktrace=1'})
    (folder / 'run.log').write_bytes(result.stdout + result.stderr)
    result.check_returncode()
    (folder / 'build.json').write_text(json.dumps(build, indent=2) + '\n')
    print(result.stdout.decode().strip())

normal, sanitized = out / 'normal', out / 'sanitized'
pngs = []
for pgm in sorted(normal.glob('*.pgm')):
    assert pgm.read_bytes() == (sanitized / pgm.name).read_bytes(), pgm.name
    if pgm.stem.endswith('-button-mask'):
        continue
    im = Image.open(pgm)
    png = out / (pgm.stem + '.png')
    im.save(png)
    pngs.append({'file': png.name, 'width': im.width, 'height': im.height,
                 'sha256': hashlib.sha256(png.read_bytes()).hexdigest()})
assert (normal / 'results.json').read_bytes() == (sanitized / 'results.json').read_bytes()
assert (normal / 'hitboxes.csv').read_bytes() == (sanitized / 'hitboxes.csv').read_bytes()
assert (normal / 'button-bounds.csv').read_bytes() == (sanitized / 'button-bounds.csv').read_bytes()
assert source_hashes == {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest() for p in identity_inputs}

for name in ['game-portrait','game-landscape','game-landscape-reverse']:
    im = Image.open(out / (name + '-x4.png')).convert('RGB')
    mask = Image.open(normal / (name + '-button-mask.pgm'))
    assert mask.size == im.size
    for y in range(im.height):
        for x in range(im.width):
            bits = mask.getpixel((x,y))
            if bits:
                color = (240,55,55) if bits & 3 else ((50,90,240) if bits & 12 else (35,190,90))
                original = im.getpixel((x,y))
                im.putpixel((x,y),tuple((a+b)//2 for a,b in zip(original,color)))
    im.save(out / (name + '-touch-overlay.png'))

# Contact sheets are labeled previews; the standalone PNGs retain exact pixels.
for suffix, names in [
    ('portrait', ['game-portrait','library','settings','battery','about','gamepad']),
    ('landscape', ['game-landscape','game-landscape-reverse','game-fullscreen'])]:
    thumb_w, thumb_h = (240, 400) if suffix == 'portrait' else (400, 240)
    sheet = Image.new('RGB', (3 * (thumb_w + 16), ((len(names)+2)//3) * (thumb_h+42)), '#dddddd')
    draw = ImageDraw.Draw(sheet)
    for i, name in enumerate(names):
        im = Image.open(out / (name + '-x4.png')).convert('RGB')
        im.thumbnail((thumb_w, thumb_h), Image.Resampling.NEAREST)
        x, y = (i % 3)*(thumb_w+16)+8, (i//3)*(thumb_h+42)+30
        sheet.paste(im, (x,y)); draw.text((x,y-20), name, fill='black')
    sheet.save(out / ('contact-' + suffix + '.png'))

receipt = {'source_head': subprocess.check_output(['git','-C',str(root),'rev-parse','HEAD'], text=True).strip(),
           'source_sha256': source_hashes, 'results': json.loads((normal / 'results.json').read_text()),
           'normal_sanitized_pixel_identity': True, 'pngs': pngs,
           'sanitizers': 'AddressSanitizer and UndefinedBehaviorSanitizer; LeakSanitizer disabled because this executor uses ptrace.',
           'fit': {'scale': '5/6', 'portrait_viewport': [15,0,450,800],
                   'landscape_viewport': [0,15,800,450],
                   'sampling': 'Actual CapGeometry: downsample electrical panel before raw display rotation; portrait X is 539-floor((464-x)*6/5), portrait Y is floor(y*6/5).',
                   'letterbox': 'white'},
           'fixture': 'Synthetic game raster and six invented library titles; no ROM executed or distributed.',
           'limits': ['Host reference only; no actual capability backend, storage, original main, or emulator linked.',
                      'Original auxiliary pages are portrait even when Game uses landscape.',
                      'Original About text still names T5S3 hardware; this is retained reference text.',
                      'Touch proof compiles actual backend geometry and original UI functions; actual device delivery remains untested.']}
(out / 'receipt.json').write_text(json.dumps(receipt, indent=2)+'\n')
print(out / 'receipt.json')
