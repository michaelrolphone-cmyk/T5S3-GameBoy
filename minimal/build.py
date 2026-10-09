#!/usr/bin/env python3
"""Build the headless Runtime port using the complete original emulator/APU.

No Arduino framework or board firmware is linked. The legacy full-source T5S3
ELF builder remains separate. Paths and byte hashes are recorded for custody.
"""
import argparse,hashlib,json,os,shutil,subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def digest(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def once(text,old,new):
    if text.count(old)!=1:raise ValueError('Core adaptation anchor changed: '+old[:70])
    return text.replace(old,new)
def stage(out):
    out=Path(out);out.mkdir(parents=True,exist_ok=True)
    shutil.copytree(ROOT/'src/crankboy_core',out/'crankboy_core',dirs_exist_ok=True)
    p=out/'crankboy_core/paperboy_crankboy_compat.h'
    p.write_text(once(p.read_text(),'#define CB_IRAM_CODE __attribute__((section(".iram1.pgb")))','#define CB_IRAM_CODE'))
    p=out/'crankboy_core/peanut_gb_core.h';core=p.read_text()
    for old,new,count in [
        ('return *(uint16_t*)ptr;','return minimal_read16(ptr);',2),
        ('*(uint16_t*)ptr = v;','minimal_write16(ptr,v);',2),
        ('return *(uint16_t*)rom_ptr;','return minimal_read16(rom_ptr);',1),
        ('*(uint64_t*)dst8 = *(const uint64_t*)src8;','memcpy(dst8,src8,sizeof(uint64_t));',1),
    ]:
        if core.count(old)!=count:raise ValueError('Core unaligned access sites changed')
        core=core.replace(old,new)
    p.write_text(core)
    text=(ROOT/'src/gbemu.c').read_text()
    text=once(text,'#include <esp_heap_caps.h>\n#include <esp_timer.h>','#include <stdlib.h>\n#include "port.h"')
    first=text.index('static void *allocate_zeroed(size_t size) {')
    last=text.index('\nstatic size_t decode_rom_size',first)
    expected='''static void *allocate_zeroed(size_t size) {
  void *buffer = heap_caps_calloc(
      1U, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (buffer == NULL) {
    buffer = heap_caps_calloc(
        1U, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  }
  if (buffer == NULL) {
    buffer = heap_caps_calloc(1U, size, MALLOC_CAP_8BIT);
  }
  return buffer;
}
'''
    if text[first:last]!=expected:raise ValueError('Core allocator changed')
    text=text[:first]+'static void *allocate_zeroed(size_t size) { return calloc(1U,size); }\n'+text[last:]
    if text.count('esp_timer_get_time()')!=6:raise ValueError('Core timer call sites changed')
    text=text.replace('esp_timer_get_time()','minimal_clock_us()')
    if text.count('heap_caps_free(')!=10:raise ValueError('Core release call sites changed')
    text=text.replace('heap_caps_free(','free(')
    (out/'gbemu.c').write_text(text)
    return out/'gbemu.c'
def sources(stage_dir):
    return [ROOT/'minimal/app.c',ROOT/'minimal/model.c',ROOT/'minimal/touch.c',stage(stage_dir),ROOT/'src/audio.c',ROOT/'src/minigb_apu/minigb_apu.c',ROOT/'minimal/audio_silent.c',ROOT/'minimal/integer.c']
def includes(runtime,stage_dir):
    return ['-I'+str(p) for p in [ROOT/'minimal',ROOT/'minimal/include',ROOT/'minimal/port',stage_dir,ROOT/'src',Path(runtime)/'sdk/app']]
def build(args):
    out=args.output.resolve();out.mkdir(parents=True,exist_ok=True)
    runtime=args.runtime.resolve();stage_dir=out/'source';src=sources(stage_dir)
    cc=args.cc;objects=[]
    common=['-std=gnu11','-O2','-fPIC','-mtext-section-literals','-mlongcalls','-fvisibility=hidden','-ffunction-sections','-fdata-sections','-fno-builtin','-DGBEMU_FAST_MONO=1',*includes(runtime,stage_dir)]
    for i,p in enumerate(src):
        obj=out/f'part-{i}.o';objects.append(obj)
        warnings=['-Wall','-Wextra','-Werror','-Wno-misleading-indentation'] if p.is_relative_to(ROOT/'minimal') else []
        subprocess.run([cc,*common,*warnings,'-c',str(p),'-o',str(obj)],check=True)
    exports={'app_main','app_module_init','app_module_fini'}
    mapping=out/'exports.map';mapping.write_text('{ global: '+'; '.join(sorted(exports))+'; local: *; };\n')
    elf=out/'gameboy.elf'
    # Pure C has no exception unwinding. GCC's non-PIC helper unwind metadata
    # cannot be relocated into this loader's packed read-only section.
    layout=out/'layout.ld'
    script=(ROOT/'riscrte/elf_loader_layout.ld').read_text().replace('    *(.eh_frame .eh_frame.*)\n','')
    script=once(script,'SECTIONS\n{','SECTIONS\n{\n  /DISCARD/ : { *(.eh_frame .eh_frame.*) }')
    layout.write_text(script)
    subprocess.run([cc,'-shared','-nostdlib','-nostartfiles','-Wl,--hash-style=sysv','-Wl,--gc-sections','-Wl,--no-relax','-Wl,--version-script='+str(mapping),'-Wl,-T,'+str(layout),*map(str,objects),'-o',str(elf)],check=True)
    symbols=subprocess.check_output([cc.removesuffix('gcc')+'nm','-D',str(elf)],text=True)
    imports={s.split()[-1] for s in symbols.splitlines() if ' U ' in ' '+s}
    actual={s.split()[-1] for s in symbols.splitlines() if len(s.split())>=3 and s.split()[-2] in ('T','D','B','R')}
    allowed={'risc_runtime_get_api','memcpy','memset','memcmp','strcmp','strlen','strncmp','strrchr','snprintf','malloc','calloc','realloc','free','strcpy','memchr'}
    if not imports<=allowed or actual!=exports:raise ValueError(f'Unexpected ELF imports/exports: {imports-allowed}, {actual}')
    validator=out/'validate-elf'
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-I'+str(runtime/'test/native_apps/stubs'),'-I'+str(runtime/'lib/elf_loader/include'),str(runtime/'lib/elf_loader/src/esp_elf_validate.c'),str(runtime/'test/native_apps/validate_test.c'),'-o',str(validator)],check=True)
    subprocess.run([str(validator),str(elf)],check=True)
    subprocess.run(['python3',str(ROOT/'riscrte/audit_elf_layout.py'),str(elf)],check=True)
    shutil.copy2(ROOT/'minimal/gameboy.json',out/'gameboy.json')
    all_sources=[p for p in (ROOT/'minimal').rglob('*') if p.is_file() and '__pycache__' not in p.parts]+[p for p in (ROOT/'src/crankboy_core').rglob('*') if p.is_file()]
    all_sources += [ROOT/n for n in ['src/gbemu.c','src/gbemu.h','src/audio.c','src/audio.h','src/minigb_apu/minigb_apu.c','src/minigb_apu/minigb_apu.h','src/paperboy_config.h','riscrte/elf_loader_layout.ld']]
    record={'schema':1,'source':subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip(),'dirty':bool(subprocess.check_output(['git','status','--porcelain'],cwd=ROOT,text=True)),'runtime_source':subprocess.check_output(['git','rev-parse','HEAD'],cwd=runtime,text=True).strip(),'runtime_header_sha256':digest(runtime/'sdk/app/RiscRuntimeV1.h'),'version':json.loads((ROOT/'minimal/gameboy.json').read_text())['version'],'compiler':subprocess.check_output([cc,'--version'],text=True).splitlines()[0],'imports':sorted(imports),'exports':sorted(exports),'elf':{'size':elf.stat().st_size,'sha256':digest(elf)},'sources':{str(p.relative_to(ROOT)):digest(p) for p in sorted(set(all_sources))},'hardware_qualified':False}
    (out/'build.json').write_text(json.dumps(record,indent=2)+'\n')
    notices=out/'licenses';notices.mkdir(exist_ok=True)
    shutil.copy2(ROOT/'src/minigb_apu/LICENSE',notices/'MiniGB-APU-LICENSE.txt')
    shutil.copy2(ROOT/'minimal/include/READER-LICENSE.txt',notices/'Reader-SDK-LICENSE.txt')
    (notices/'PeanutGB-CrankBoy-notice.txt').write_text((ROOT/'src/crankboy_core/peanut_gb.h').read_text().split('*/',1)[0]+'*/\n')
    (notices/'font8x8-notice.txt').write_text((ROOT/'minimal/include/font8x8_basic.h').read_text().split('*/',1)[0]+'*/\n')
    print(json.dumps({'elf':record['elf'],'imports':record['imports']}))
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--runtime',type=Path,required=True);p.add_argument('--output',type=Path,default=ROOT/'dist/minimal');p.add_argument('--cc',default=os.environ.get('NATIVE_APP_CC','xtensa-esp32s3-elf-gcc'));build(p.parse_args())
