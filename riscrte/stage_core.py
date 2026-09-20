#!/usr/bin/env python3
"""Stage the original core unchanged except for the ELF loader's section map."""
from pathlib import Path
from prepare_elf import ROOT, patch_once


def stage_core(destination: Path) -> None:
    core = destination / 'crankboy_core'
    core.mkdir(parents=True, exist_ok=True)
    (destination / 'gbemu.c').write_bytes((ROOT / 'src/gbemu.c').read_bytes())
    (core / 'peanut_gb.h').write_bytes(
        (ROOT / 'src/crankboy_core/peanut_gb.h').read_bytes())
    compat = (ROOT / 'src/crankboy_core/paperboy_crankboy_compat.h').read_text()
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
    (core / 'paperboy_crankboy_compat.h').write_text(compat)
    print('Staged original emulator core with ELF-mappable executable section')


if __name__ == '__main__':
    stage_core(ROOT / 'build/riscrte/src')
