#!/usr/bin/env python3
"""Build GameBoy ELF and reject imports/relocations its RiscRTE loader cannot map."""
import json
import os
import pathlib
import re
import shutil
import struct
import subprocess

ROOT = pathlib.Path(__file__).resolve().parents[1]
OUT = ROOT / "dist" / "riscrte"
OUT.mkdir(parents=True, exist_ok=True)

cc = os.environ.get("NATIVE_APP_CC") or shutil.which("xtensa-esp32s3-elf-gcc")
if not cc:
    core = pathlib.Path(os.environ.get("PLATFORMIO_CORE_DIR", pathlib.Path.home() / ".platformio"))
    cc = str(core / "packages/toolchain-xtensa-esp32s3/bin/xtensa-esp32s3-elf-gcc")
readelf = cc.replace("gcc", "readelf")

sources = [
    ROOT / "riscrte/gameboy_app.c",
    ROOT / "src/gbemu.c",
    ROOT / "riscrte/audio_stub.c",
    ROOT / "riscrte/div64.c",
    ROOT / "riscrte/libc_compat.c",
]
output = OUT / "gameboy.elf"
cmd = [
    cc, "-std=c11", "-Os", "-DRISCRTE_ELF_APP=1", "-fPIC",
    "-mtext-section-literals", "-mlongcalls", "-fvisibility=hidden",
    "-nostdlib", "-nostartfiles", "-shared", "-Wl,--hash-style=sysv",
    "-I" + str(ROOT / "riscrte/include"),
    "-I" + str(ROOT / "src"),
    *map(str, sources), "-o", str(output),
]
print(" ".join(cmd), flush=True)
subprocess.run(cmd, check=True)

symbols = subprocess.check_output([readelf, "--dyn-syms", "--wide", str(output)], text=True)
if not any("GLOBAL" in line and "FUNC" in line and "UND" not in line and line.split()[-1] == "app_main"
           for line in symbols.splitlines() if line.strip()):
    raise SystemExit("gameboy.elf does not export app_main")

undefined = set()
for line in symbols.splitlines():
    fields = line.split()
    if len(fields) >= 8 and fields[4] == "GLOBAL" and fields[6] == "UND":
        undefined.add(fields[7])

contract_path = ROOT / "riscrte/riscrte-symbols.json"
contract = json.loads(contract_path.read_text(encoding="utf-8"))
required = {item["name"] for item in contract["required_exports"]}
undeclared = sorted(undefined - required)
unused = sorted(required - undefined)
if undeclared:
    raise SystemExit("ELF has undeclared RiscRTE imports: " + ", ".join(undeclared))
if unused:
    raise SystemExit("Symbol manifest contains imports no longer required by ELF: " + ", ".join(unused))

# RiscRTE's CONFIG_ELF_LOADER_BUS_ADDRESS_MIRROR section loader explicitly
# maps only .text, .rodata, .data, .data.rel.ro, and .bss. It neither loads
# nor maps an independent .iram1.pgb section, even if PT_LOAD contains it.
# Audit both relocation *sites* and RELATIVE pointer *targets* so readelf's
# ordinary success cannot conceal an ELF that will fail before app_main().
section_report = subprocess.check_output([readelf, "-SW", str(output)], text=True)
section_re = re.compile(
    r"^\s*\[\s*\d+\]\s+(\S+)\s+(\S+)\s+([0-9a-fA-F]+)\s+"
    r"([0-9a-fA-F]+)\s+([0-9a-fA-F]+)\s+\S+\s+(\S+)", re.M,
)
sections = []
for match in section_re.finditer(section_report):
    name, kind, address, offset, size, flags = match.groups()
    sections.append((name, kind, int(address, 16), int(offset, 16), int(size, 16), flags))
if not sections:
    raise SystemExit("Could not parse ELF section headers for loader compatibility audit")

mapped_names = {".text", ".rodata", ".data", ".data.rel.ro", ".bss"}
for name, kind, address, offset, size, flags in sections:
    if size and "A" in flags and "X" in flags and name != ".text":
        raise SystemExit(
            f"Unmapped executable section {name} at 0x{address:08x}: "
            "RiscRTE's loader maps executable code only from .text"
        )


def owner(address):
    return next(
        (section for section in sections
         if section[4] and section[2] <= address < section[2] + section[4]),
        None,
    )


relocs = subprocess.check_output([readelf, "-rW", str(output)], text=True)
reloc_re = re.compile(r"^\s*([0-9a-fA-F]{8})\s+\S+\s+(R_XTENSA_\S+)", re.M)
entries = [(int(match.group(1), 16), match.group(2)) for match in reloc_re.finditer(relocs)]
if not entries:
    raise SystemExit("ELF has no relocation records; cannot validate loader compatibility")

image = output.read_bytes()
relative_count = 0
for address, relocation_type in entries:
    site = owner(address)
    if site is None or site[0] not in mapped_names:
        name = site[0] if site else "no allocated section"
        raise SystemExit(f"Unmappable {relocation_type} site 0x{address:08x} in {name}")
    if relocation_type not in {"R_XTENSA_RELATIVE", "R_XTENSA_RTLD",
                               "R_XTENSA_JMP_SLOT", "R_XTENSA_GLOB_DAT"}:
        raise SystemExit(f"Unsupported RiscRTE relocation type: {relocation_type}")
    if relocation_type != "R_XTENSA_RELATIVE":
        continue
    relative_count += 1
    name, kind, section_address, file_offset, size, flags = site
    if kind == "NOBITS" or file_offset + address - section_address + 4 > len(image):
        raise SystemExit(f"RELATIVE relocation site 0x{address:08x} has no file-backed word")
    pointer = struct.unpack_from("<I", image, file_offset + address - section_address)[0]
    if pointer == 0:
        continue
    target = owner(pointer)
    if target is None or target[0] not in mapped_names:
        name = target[0] if target else "no allocated section"
        raise SystemExit(
            f"Unmappable RELATIVE target 0x{pointer:08x} at 0x{address:08x} in {name}"
        )

app_manifest = ROOT / "riscrte/gameboy.json"
shutil.copyfile(app_manifest, OUT / "gameboy.json")
shutil.copyfile(contract_path, OUT / "riscrte-symbols.json")
print(symbols)
print(f"Validated {len(entries)} relocation sites and {relative_count} relative targets against RiscRTE section loader")
print(f"Built {output} with exact declared imports: {', '.join(sorted(undefined))}")
