#!/usr/bin/env python3
"""Run staged original application, backend and emulator against host providers."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]
p = argparse.ArgumentParser()
p.add_argument('--runtime', type=Path, required=True)
p.add_argument('--system', type=Path, required=True)
p.add_argument('--sdk', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
p.add_argument('--sanitize', action='store_true')
p.add_argument('--real-runtime', action='store_true')
p.add_argument('--backend-source', type=Path)
p.add_argument('--gt911-root', type=Path)
p.add_argument('--case', action='append')
a = p.parse_args()
out = a.output.resolve(); out.mkdir(parents=True, exist_ok=True)
stage = out / 'source'
def module(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m); return m
original = module(ROOT/'capability/stage.py', 'original_stage')
old = module(ROOT/'minimal/build.py', 'core_stage')
main = original.stage(stage); core = old.stage(stage)
assert 'wait_epd_idle' not in main.read_text()
assert 'wait_vsync_frames' not in main.read_text()
text = core.read_text().replace('#include "port.h"', '#include "port.h"\nvoid *cap_core_calloc(size_t,size_t);\nvoid cap_core_free(void *);')
text = text.replace('return calloc(1U,size);','return cap_core_calloc(1U,size);').replace('free(', 'cap_core_free(').replace('void cap_core_cap_core_free(', 'void cap_core_free(')
core.write_text(text+"\n"+(ROOT/"capability/tests/normalize_state.inc").read_text())
backend = a.backend_source or ROOT/'capability/backend.cpp'
cpp = [main,backend,ROOT/'capability/storage.cpp',stage/'paperboy_ui.cpp',stage/'paperboy_game_clock.cpp']+[ROOT/'src'/n for n in ['mono_canvas.cpp','paperboy_landscape.cpp','paperboy_orientation.cpp','paperboy_controller_navigation.cpp','builtin_demo_rom.cpp']]
c = [core,ROOT/'src/audio.c',ROOT/'src/minigb_apu/minigb_apu.c',ROOT/'minimal/audio_silent.c',ROOT/'minimal/integer.c',stage/'rom_port.c']
c += [a.system/'lib/PortableApps/src'/n for n in ['PortableTimeZone.c','PortableTimeZoneCatalog.c','PortableTimeZonePreference.c']]
# Compile the target's three app-local libc helpers under private test names.
# Every application call uses them; sanitizer/libstdc++ retain their host libc.
c += [ROOT/'capability/libc_compat.c']
gt911_sources=[]
if a.gt911_root:
    gt911_sources=[a.gt911_root/p for p in ['minimal/test/hid_gt911_backend.c','minimal/drivers/x4pro_gt911/driver.c']]
    c += gt911_sources
include = [ROOT/'capability/include',ROOT/'capability',a.sdk,ROOT/'minimal/include',ROOT/'minimal',ROOT/'minimal/port',stage,ROOT/'src',ROOT/'riscrte',a.runtime/'sdk/app',a.system/'lib/PortableApps/include']
if a.real_runtime:
    include += [a.runtime/p for p in ['src','sdk/driver','sdk/hardware','lib/ArduinoJson/src','test/drivers/stubs']]
flags = ['-O1','-g','-DGBEMU_FAST_MONO=1','-DPAPERBOY_FIRMWARE_VERSION="1.3.24"','-Wall','-Wextra','-Wno-unused-function','-Wno-misleading-indentation']+['-I'+str(v) for v in include]
if a.gt911_root:flags+=['-DGAMEBOY_REAL_GT911']
if a.sanitize: flags += ['-fsanitize=address,undefined','-fno-omit-frame-pointer','-fno-pie','-no-pie']
objects = []
commands = []
for i, src in enumerate(c+cpp):
    obj = out/f'part-{i}.o'; objects.append(obj)
    opts = [] if src in gt911_sources else ['-include',str(ROOT/'capability/tests/intercept.hpp')]
    if a.real_runtime and src == backend: opts += ['-Drisc_runtime_get_api=gameboy_runtime_get_api','-Dapp_module_init=gameboy_module_init','-Dapp_main=gameboy_main','-Dapp_module_fini=gameboy_module_fini']
    if src == main: opts += ['-Dmono_clear=lifecycle_clear','-Daudio_service_frame=lifecycle_audio_service_frame','-Dgbemu_run_frame=lifecycle_run_frame','-Dpaperboy_ui_draw_page=lifecycle_draw_page','-Dpaperboy_ui_map_actions=lifecycle_map_actions']
    command = [('c++' if src.suffix == '.cpp' else 'cc'),('-std=c++17' if src.suffix=='.cpp' else '-std=gnu11'),*flags,*opts,'-c',str(src),'-o',str(obj)]
    commands.append(command)
    result = subprocess.run(command, text=True, capture_output=True)
    if result.returncode: raise RuntimeError(' '.join(command)+'\n'+result.stdout+result.stderr)
exe = out/'lifecycle'
runtime_sources=[]
if a.real_runtime:
    runtime_sources=[a.runtime/p for p in ['src/bootstrap/Json.cpp','src/bootstrap/Board.cpp','src/bootstrap/Runtime.cpp','src/runtime/streams/AppStreamSessions.cpp','src/runtime/streams/ProviderQueueHost.cpp','src/runtime/drivers/ProviderGraphV2.cpp','src/runtime/drivers/ProviderModuleV2.cpp']]
native_includes=['-I'+str(a.runtime/p) for p in ['src','sdk/app','sdk/driver','sdk/hardware','lib/ArduinoJson/src','test/drivers/stubs']] if a.real_runtime else []
if a.real_runtime:
    for i,src in enumerate(runtime_sources+[ROOT/'capability/tests/runtime_phase_host.cpp']):
        obj=out/f'native-{i}.o';objects.append(obj)
        command=['c++','-std=c++17',*native_includes,*[v for v in flags if not v.startswith('-I')],'-c',str(src),'-o',str(obj)]
        commands.append(command);result=subprocess.run(command,text=True,capture_output=True)
        if result.returncode:raise RuntimeError(result.stdout+result.stderr)
command = ['c++','-std=c++17',*flags,*(['-DGAMEBOY_REAL_RUNTIME','-rdynamic'] if a.real_runtime else []),str(ROOT/'capability/tests/lifecycle.cpp'),*map(str,objects),*(['-ldl'] if a.real_runtime else []),'-o',str(exe)]
commands.append(command)
result = subprocess.run(command,text=True,capture_output=True)
if result.returncode: raise RuntimeError(result.stdout+result.stderr)
(out/'commands.json').write_text(json.dumps(commands,indent=2)+'\n')
cases = a.case or (['journey'] if a.real_runtime else ['journey','slow-journey','file-handoff','input-error-pending','display-terminal','display-terminal-vsync','display-terminal-ready','storage-terminal','realtime-terminal','touch-release-terminal','config-terminal','file-handoff-terminal','launch-terminal','save-terminal','rom-open-failure','rom-read-failure','rom-init-failure','rom-retry','timing-fast','timing-lcd','timing-slow','timing-compute-fast','timing-compute-slow','mailbox-latest','mailbox-quit','header-load-disk','header-load-memory','header-load-missing','header-load-corrupt','header-save-close-retained','header-load-close-retained','header-load-read-failure','header-load-display-retained'])
results = []
for case in cases:
    folder = out/case; folder.mkdir(exist_ok=True)
    if a.real_runtime:
        subprocess.run(['cc','-std=c11','-fPIC','-shared',str(ROOT/'capability/tests/runtime_phase_app.c'),'-o',str(folder/'gameboy.elf')],check=True)
    result = subprocess.run([str(exe),case,str(folder)],text=True,capture_output=True,timeout=60,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0:abort_on_error=1','UBSAN_OPTIONS':'halt_on_error=1:print_stacktrace=1'})
    (folder/'run.log').write_text(result.stdout+result.stderr)
    print(result.stdout+result.stderr,end='')
    if result.returncode: raise RuntimeError(f'{case}: exit {result.returncode}; see {folder}/run.log')
    from PIL import Image
    for image in folder.glob('*.pbm'):
        Image.open(image).save(image.with_suffix('.png'))
    results.append(case)
(out/'receipt.json').write_text(json.dumps({'source':subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip(),'dirty':bool(subprocess.check_output(['git','status','--porcelain'],cwd=ROOT,text=True)),'sources':{str(v):hashlib.sha256(v.read_bytes()).hexdigest() for v in c+cpp+[ROOT/'capability/tests/lifecycle.cpp',ROOT/'capability/tests/intercept.hpp']},'cases':results,'sanitized':a.sanitize,'staged_sources':{str(v.relative_to(stage)):hashlib.sha256(v.read_bytes()).hexdigest() for v in stage.rglob('*') if v.is_file()},'fixture':'Actual staged original main, UI, controller, emulator, backend and storage; deterministic host providers and built-in demo ROM.'},indent=2)+'\n')

comparisons=[]
for baseline,others in [('timing-fast',['timing-lcd','timing-slow']),('timing-compute-fast',['timing-compute-slow'])]:
    if baseline not in results:continue
    expected=json.loads((out/baseline/'simulation.json').read_text())
    for case in others:
        if case not in results:continue
        actual=json.loads((out/case/'simulation.json').read_text())
        assert actual['steps']==expected['steps'],(baseline,case,'logical history differs')
        assert (out/case/'final-state.bin').read_bytes()==(out/baseline/'final-state.bin').read_bytes(),(baseline,case,'CPU/APU final state differs')
        comparisons.append({'baseline':baseline,'case':case,'frames':actual['frames'],'equal_input_times_and_state_history':True,'equal_final_cpu_apu_state':True})
(out/'timing-comparisons.json').write_text(json.dumps(comparisons,indent=2)+'\n')
