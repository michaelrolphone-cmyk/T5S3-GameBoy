#!/usr/bin/env python3
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

allowed = {
    "t5_app_get_api", "t5_storage_get_api", "malloc", "calloc", "realloc", "free",
    "memset", "memcpy", "strlen", "clock_gettime", "puts", "printf",
}
undefined = set()
for line in symbols.splitlines():
    fields = line.split()
    if len(fields) >= 8 and fields[4] == "GLOBAL" and fields[6] == "UND":
        undefined.add(fields[7])
missing = sorted(undefined - allowed)
if missing:
    raise SystemExit("ELF imports symbols not exported by current RiscRTE app loader: " + ", ".join(missing))

manifest = ROOT / "riscrte/gameboy.json"
shutil.copyfile(manifest, OUT / "gameboy.json")
print(symbols)
print(f"Built {output} with imports: {', '.join(sorted(undefined))}")
