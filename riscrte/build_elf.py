#!/usr/bin/env python3
import json
import os
import pathlib
import shutil
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
    cc, "-std=c11", "-Os", "-fPIC", "-mtext-section-literals", "-mlongcalls",
    "-fvisibility=hidden", "-nostdlib", "-nostartfiles", "-shared",
    "-Wl,--hash-style=sysv",
    "-I" + str(ROOT / "riscrte/include"),
    "-I" + str(ROOT / "src"),
    *map(str, sources), "-o", str(output),
]
print(" ".join(cmd))
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

app_manifest = ROOT / "riscrte/gameboy.json"
shutil.copyfile(app_manifest, OUT / "gameboy.json")
shutil.copyfile(contract_path, OUT / "riscrte-symbols.json")
print(symbols)
print(f"Built {output} with exact declared imports: {', '.join(sorted(undefined))}")
