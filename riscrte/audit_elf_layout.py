#!/usr/bin/env python3
"""Audit the produced GameBoy ELF against the existing RiscRTE section loader.

CI previously checked symbols but accepted separate .iram1.*, .ctors and .dtors
sections. The firmware maps only canonical sections, so those ELF files build
successfully and then fail during dlopen. Check the *linked artifact* before
publishing its integrity manifest; this is an ELF-side compatibility contract.
"""
import struct
import sys
from pathlib import Path

ELF_HEADER = struct.Struct('<16sHHIIIIIHHHHHH')
SECTION = struct.Struct('<IIIIIIIIII')
RELA = struct.Struct('<IIi')
SYMBOL = struct.Struct('<IIIBBH')
MAPPED = {'.text', '.rodata', '.data', '.data.rel.ro', '.bss'}
# These are read from the ELF file by the loader before and during relocation,
# not loaded as application memory. The .dynamic table is loader-only on RiscRTE.
LOADER_METADATA = {'.hash', '.dynsym', '.dynstr', '.rela.dyn', '.rela.plt',
                   '.dynamic', '.xt.lit', '.xt.prop'}


def audit(path):
    blob = path.read_bytes()
    if len(blob) < ELF_HEADER.size:
        raise ValueError('truncated ELF header')
    hdr = ELF_HEADER.unpack_from(blob)
    ident, elf_type, machine = hdr[:3]
    if ident[:7] != b'\x7fELF\x01\x01\x01' or elf_type != 3 or machine != 94:
        raise ValueError('expected 32-bit little-endian Xtensa ET_DYN ELF')
    entry, shoff, shentsize, shnum, shstrndx = hdr[4], hdr[6], hdr[11], hdr[12], hdr[13]
    if shentsize != SECTION.size or not shnum or shstrndx >= shnum or shoff + shnum * shentsize > len(blob):
        raise ValueError('invalid ELF section table')
    sections = [SECTION.unpack_from(blob, shoff + i * shentsize) for i in range(shnum)]
    shstr = sections[shstrndx]
    if shstr[4] + shstr[5] > len(blob):
        raise ValueError('invalid ELF section-name table')
    names = blob[shstr[4]:shstr[4] + shstr[5]]

    def section_name(s):
        if s[0] >= len(names):
            raise ValueError('invalid ELF section name offset')
        end = names.find(b'\0', s[0])
        if end < 0:
            raise ValueError('unterminated ELF section name')
        return names[s[0]:end].decode('ascii')

    entries = [(section_name(s), s) for s in sections]
    mapped = {}
    for name, s in entries:
        if name in MAPPED and s[5] and (s[2] & 2):
            if name in mapped:
                raise ValueError(f'duplicate mapped section {name}')
            if s[3] + s[5] > 0xffffffff:
                raise ValueError(f'overflowed mapped section {name}')
            mapped[name] = s
    if '.text' not in mapped or '.data' not in mapped or '.rodata' not in mapped:
        raise ValueError('missing canonical text, data or rodata section')
    if mapped['.text'][2] & 4 == 0 or mapped['.data'][2] & 1 == 0:
        raise ValueError('incorrect executable or writable section flags')
    if not mapped['.text'][3] <= entry < mapped['.text'][3] + mapped['.text'][5]:
        raise ValueError('ELF entry is outside .text')

    # The current host packs these sections consecutively, ignoring sh_addralign.
    # Audit runtime offsets, not only linked virtual addresses (which are aligned).
    packed_offset = 0
    for name in ('.data', '.rodata', '.data.rel.ro', '.bss'):
        section = mapped.get(name)
        if section is None:
            continue
        alignment = max(4, section[8])
        if alignment > 16 or packed_offset % alignment or section[3] % alignment:
            raise ValueError(f'{name} misaligned in RiscRTE packed data at offset '
                             f'{packed_offset:#x}, requires {alignment}-byte alignment; '
                             f'packed sections: '
                             f'{[(n, hex(mapped[n][5]), mapped[n][8]) for n in (".data", ".rodata", ".data.rel.ro", ".bss") if n in mapped]}')
        packed_offset += section[5]

    def location(addr):
        return next((name for name, s in mapped.items()
                     if s[3] <= addr < s[3] + s[5]), None)

    # The actual firmware maps only MAPPED by name. A separately allocated
    # application section is never copied to RAM and must fail at build time,
    # even when its first execution/lookup happens after dlopen succeeds.
    for name, s in entries:
        if not s[5] or not s[2] & 2 or name in MAPPED or name in LOADER_METADATA:
            continue
        if s[3] >= mapped['.text'][3] and (s[2] & 4 or s[2] & 1 or s[1] == 1):
            raise ValueError(f'unmapped allocated runtime section {name} at {s[3]:#x}')

    count = 0
    for name, s in entries:
        if s[1] != 4:  # SHT_RELA
            continue
        if s[4] + s[5] > len(blob) or s[5] % RELA.size:
            raise ValueError(f'invalid relocation section {name}')
        for off in range(s[4], s[4] + s[5], RELA.size):
            target, info, _ = RELA.unpack_from(blob, off)
            if (info & 0xff) != 5:  # R_XTENSA_RELATIVE
                continue
            count += 1
            site_section = location(target)
            if site_section is None:
                raise ValueError(f'R_XTENSA_RELATIVE site {target:#x} unmappable in {name}')
            if target % 4 or target + 4 > mapped[site_section][3] + mapped[site_section][5]:
                raise ValueError(f'invalid R_XTENSA_RELATIVE site {target:#x}')
            mapped_section = mapped[site_section]
            offset = mapped_section[4] + target - mapped_section[3]
            if mapped_section[1] == 8 or offset + 4 > len(blob):
                raise ValueError(f'relocation at invalid file offset {offset:#x}')
            value = struct.unpack_from('<I', blob, offset)[0]
            if value and location(value) is None:
                raise ValueError(f'R_XTENSA_RELATIVE target {value:#x} from site {target:#x} unmappable')
    print(f'RiscRTE ELF layout PASS: {len(mapped)} mapped sections, {count} verified relative relocations')


if __name__ == '__main__':
    try:
        audit(Path(sys.argv[1]))
    except (IndexError, OSError, ValueError, struct.error) as exc:
        raise SystemExit(f'RiscRTE ELF layout FAIL: {exc}') from exc
