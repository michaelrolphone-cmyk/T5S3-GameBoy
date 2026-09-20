#!/usr/bin/env python3
"""Stage the entire original core; alter only its ELF-incompatible IRAM section."""
from pathlib import Path
import shutil
from prepare_elf import ROOT, patch_once


def stage_core(destination: Path) -> None:
    core = destination / 'crankboy_core'
    # Includes nested pgb/include and the other CrankBoy headers; copying just
    # peanut_gb.h omits dependencies and breaks the real Xtensa compilation.
    shutil.copytree(ROOT / 'src/crankboy_core', core, dirs_exist_ok=True)
    shutil.copy2(ROOT / 'src/gbemu.c', destination / 'gbemu.c')
    compat = (core / 'paperboy_crankboy_compat.h').read_text(encoding='utf-8')
    compat = patch_once(
        compat,
        '#define CB_IRAM_CODE __attribute__((section(".iram1.pgb")))',
        '#ifdef PAPERBOY_RISCRTE_ELF\n'
        '// The loader maps executable code from .text, not .iram1.pgb.\n'
        '#define CB_IRAM_CODE\n'
        '#else\n'
        '#define CB_IRAM_CODE __attribute__((section(".iram1.pgb")))\n'
        '#endif',
        'core ELF section')
    (core / 'paperboy_crankboy_compat.h').write_text(compat, encoding='utf-8')
    print('Staged complete original CrankBoy source/include tree with ELF-mappable code')


if __name__ == '__main__':
    stage_core(ROOT / 'build/riscrte/src')
