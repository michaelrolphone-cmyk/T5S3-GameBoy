"""Regression for aligned virtual sections becoming misaligned after host packing."""
from pathlib import Path
import sys
import tempfile
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'riscrte'))
from audit_elf_layout import audit, ELF_HEADER, SECTION


def fixture(rodata_size):
    names = b'\0.text\0.rodata\0.data\0.bss\0.shstrtab\0'
    sections = [(0,) * 10]
    blob = bytearray(512)
    for name, kind, flags, addr, offset, size, alignment in [
        (b'.text', 1, 6, 0x1000, 64, 16, 4),
        (b'.rodata', 1, 2, 0x2000, 80, rodata_size, 4),
        (b'.data', 1, 3, 0x3000, 128, 16, 4),
        (b'.bss', 8, 3, 0x4000, 144, 32, 8),
        (b'.shstrtab', 3, 0, 0, 160, len(names), 1),
    ]:
        sections.append((names.index(name), kind, flags, addr, offset, size, 0, 0, alignment, 0))
    blob[160:160 + len(names)] = names
    blob[:ELF_HEADER.size] = ELF_HEADER.pack(b'\x7fELF\x01\x01\x01' + bytes(9),
        3, 94, 1, 0x1000, 0, 256, 0, ELF_HEADER.size, 0, 0, SECTION.size, len(sections), 5)
    for i, section in enumerate(sections):
        start = 256 + i * SECTION.size
        blob[start:start + SECTION.size] = SECTION.pack(*section)
    return blob


class LayoutTest(unittest.TestCase):
    def test_packed_alignment(self):
        with tempfile.TemporaryDirectory() as directory:
            file = Path(directory) / 'gameboy.elf'
            file.write_bytes(fixture(14))
            with self.assertRaisesRegex(ValueError, '.bss misaligned'):
                audit(file)
            file.write_bytes(fixture(16))
            audit(file)


if __name__ == '__main__':
    unittest.main()
