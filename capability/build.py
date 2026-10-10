#!/usr/bin/env python3
"""Build the original Paperboy application with a capability-only platform."""
import argparse,hashlib,importlib.util,json,os,shutil,subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def module(path,name):
 s=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m
def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def revision(path):
 result=subprocess.run(['git','rev-parse','HEAD'],cwd=path,text=True,capture_output=True)
 return result.stdout.strip() if result.returncode==0 else None
def build(a):
 out=a.output.resolve();out.mkdir(parents=True,exist_ok=True);stage=out/'source';stage.mkdir(exist_ok=True)
 original=module(ROOT/'capability/stage.py','original_stage');old=module(ROOT/'minimal/build.py','core_stage')
 baseline=json.loads((ROOT/'capability/reconstruction/baseline-1323-build-receipt.json').read_text())
 for name,want in baseline['sources'].items():
  if name.startswith('src/') and sha(ROOT/name)!=want:raise ValueError('Original core/UI source changed: '+name)
 main=original.stage(stage);core=old.stage(stage)
 text=core.read_text();text=text.replace('#include "port.h"','#include "port.h"\nvoid *cap_core_calloc(size_t,size_t);\nvoid cap_core_free(void *);')
 text=text.replace('return calloc(1U,size);','return cap_core_calloc(1U,size);').replace('free(', 'cap_core_free(').replace('void cap_core_cap_core_free(', 'void cap_core_free(')
 core.write_text(text)
 cpp=[main,ROOT/'capability/backend.cpp',ROOT/'capability/storage.cpp',stage/'paperboy_ui.cpp',stage/'paperboy_game_clock.cpp']+[ROOT/'src'/n for n in ['mono_canvas.cpp','paperboy_landscape.cpp','paperboy_orientation.cpp','paperboy_controller_navigation.cpp','builtin_demo_rom.cpp']]
 c=[core,ROOT/'src/audio.c',ROOT/'src/minigb_apu/minigb_apu.c',ROOT/'minimal/audio_silent.c',ROOT/'minimal/integer.c',stage/'rom_port.c',ROOT/'capability/libc_compat.c']
 c += [a.system/'lib/PortableApps/src'/n for n in ['PortableTimeZone.c','PortableTimeZoneCatalog.c','PortableTimeZonePreference.c']]
 include=[ROOT/'capability/include',ROOT/'capability',a.sdk,ROOT/'minimal/include',ROOT/'minimal',ROOT/'minimal/port',stage,ROOT/'src',ROOT/'riscrte',a.runtime/'sdk/app',a.sdk,a.system/'lib/PortableApps/include']
 flags=['-O2','-fPIC','-mtext-section-literals','-mlongcalls','-fvisibility=hidden','-ffunction-sections','-fdata-sections','-fno-builtin','-DGBEMU_FAST_MONO=1','-DPAPERBOY_FIRMWARE_VERSION="1.3.24"']+['-I'+str(p) for p in include]
 objects=[]
 for i,p in enumerate(c+cpp):
  if a.compile_available and not p.exists():continue
  obj=out/f'part-{i}.o';objects.append(obj);is_cpp=p.suffix=='.cpp';compiler=a.cc.removesuffix('gcc')+'g++' if is_cpp else a.cc
  options=['-std=gnu++17','-fno-exceptions','-fno-rtti','-fno-threadsafe-statics','-fno-use-cxa-atexit'] if is_cpp else ['-std=gnu11']
  subprocess.run([compiler,*flags,*options,'-Wall','-Wextra','-MMD','-MF',str(out/f'part-{i}.d'),'-c',str(p),'-o',str(obj)],check=True)
 if a.compile_available:return
 exports={'app_main','app_module_init','app_module_fini'};mapping=out/'exports.map';mapping.write_text('{ global: '+'; '.join(sorted(exports))+'; local: *; };\n')
 layout=out/'layout.ld';script=(ROOT/'riscrte/elf_loader_layout.ld').read_text().replace('    *(.eh_frame .eh_frame.*)\n','');script=old.once(script,'SECTIONS\n{','SECTIONS\n{\n  /DISCARD/ : { *(.eh_frame .eh_frame.*) }');layout.write_text(script)
 elf=out/'gameboy.elf'
 subprocess.run([a.cc,'-shared','-nostdlib','-nostartfiles','-Wl,--hash-style=sysv','-Wl,--gc-sections','-Wl,--no-relax','-Wl,--version-script='+str(mapping),'-Wl,-T,'+str(layout),*map(str,objects),'-o',str(elf)],check=True)
 symbols=subprocess.check_output([a.cc.removesuffix('gcc')+'nm','-D',str(elf)],text=True);imports={line.split()[-1] for line in symbols.splitlines() if ' U ' in ' '+line}
 allowed={'risc_runtime_get_api','memcpy','memset','memcmp','memmove','strcmp','strlen','strncmp','strrchr','strchr','snprintf','vsnprintf','malloc','calloc','realloc','free','strcpy','strncpy','memchr','strtol','strtoul','strtok_r','strcspn','toupper','tolower','abs'}
 if not imports<=allowed:raise ValueError('Unexpected imports: '+str(imports-allowed))
 validator=out/'validate-elf';subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-I'+str(a.runtime/'test/native_apps/stubs'),'-I'+str(a.runtime/'lib/elf_loader/include'),str(a.runtime/'lib/elf_loader/src/esp_elf_validate.c'),str(a.runtime/'test/native_apps/validate_test.c'),'-o',str(validator)],check=True);subprocess.run([str(validator),str(elf)],check=True)
 subprocess.run([os.environ.get('ELF_AUDIT_PYTHON','python3'),str(ROOT/'riscrte/audit_elf_layout.py'),str(elf)],check=True)
 manifest=json.loads((ROOT/'minimal/gameboy.json').read_text());manifest['version']='1.3.24';manifest['requires'] += [{'capability':n,'api':1} for n in ['board.battery','runtime.realtime','storage.key-value']];(out/'gameboy.json').write_text(json.dumps(manifest,indent=2)+'\n')
 tracked=[p for folder in [ROOT/'src',ROOT/'capability',ROOT/'minimal/port',ROOT/'minimal/include'] for p in folder.rglob('*') if p.is_file() and '__pycache__' not in p.parts and 'render-output' not in p.parts]
 record={'schema':1,'source':subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip(),'dirty':bool(subprocess.check_output(['git','status','--porcelain'],cwd=ROOT,text=True)),'runtime_source':subprocess.check_output(['git','rev-parse','HEAD'],cwd=a.runtime,text=True).strip(),'system_source':revision(a.system),'version':'1.3.24','compiler':subprocess.check_output([a.cc,'--version'],text=True).splitlines()[0],'elf':{'size':elf.stat().st_size,'sha256':sha(elf)},'imports':sorted(imports),'sources':{str(p.relative_to(ROOT)):sha(p) for p in sorted(set(tracked))},'staged_sources':{str(p.relative_to(stage)):sha(p) for p in sorted(stage.rglob('*')) if p.is_file()},'hardware_qualified':False}
 record['reconstructed']=True
 record['recovered_predecessor']='9424f9e225a8351fa38e1956469221047999d03a'
 record['not_recovered_revision']='e429db588040c19d6ca152066c36a32a564afa0a'
 deps=set()
 for dep in out.glob('part-*.d'):
  content=dep.read_text().replace('\\\n',' ')
  deps.update(Path(v) for v in content.split(':',1)[1].split() if Path(v).is_file())
 record['compiled_dependencies']={str(p):sha(p) for p in sorted(deps)}
 record['dependency_files']={str(p):sha(p) for p in sorted(set(c+cpp+[p for folder in [a.sdk,a.system/'lib/PortableApps/include',a.runtime/'sdk/app'] for p in folder.rglob('*.h') if p.is_file()]))}
 (out/'build.json').write_text(json.dumps(record,indent=2)+'\n');print(json.dumps(record['elf']))
if __name__=='__main__':
 p=argparse.ArgumentParser();p.add_argument('--runtime',type=Path,required=True);p.add_argument('--system',type=Path,required=True);p.add_argument('--sdk',type=Path,required=True);p.add_argument('--output',type=Path,required=True);p.add_argument('--cc',default=os.environ.get('NATIVE_APP_CC','xtensa-esp32s3-elf-gcc'));p.add_argument('--compile-available',action='store_true');build(p.parse_args())
