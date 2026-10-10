#!/usr/bin/env python3
"""Measure selected-ROM work separately from scan through actual SD/FatFs."""
import argparse,os,subprocess,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser()
for n in ('x4','runtime','reader','output'):p.add_argument('--'+n,type=Path,required=True)
p.add_argument('--stat-contract',action='store_true');p.add_argument('--sanitize',action='store_true');p.add_argument('--size',type=int,action='append')
a=p.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
subprocess.run([sys.executable,str(a.x4/'minimal/scripts/prepare_sdk.py'),'--runtime',str(a.runtime),'--reader',str(a.reader),'--output',str(out/'sdk')],check=True)
incs=[out/'sdk',a.reader/'Drivers/storage_fatfs',a.reader/'Drivers/x4pro_board',a.reader,ROOT/'capability',ROOT/'src',ROOT/'riscrte']
flags=['-O1','-g','-Wall','-Wextra','-Werror','-Wno-overflow','-DX4_EXPECT_BATCHING=1','-DX4_SD_TEST_SOURCE="'+str(a.x4.resolve()/'minimal/test/sd_test.c')+'"']
if a.sanitize:flags+=['-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-omit-frame-pointer','-fno-pie','-no-pie']
objects=[]
sources=[ROOT/'tests/capability_provider_load_bridge.c',a.reader/'Drivers/storage_fatfs/fatfs/ff.c',a.reader/'Drivers/storage_fatfs/fatfs/ffunicode.c',ROOT/'riscrte/rom_port.c',ROOT/'capability/storage.cpp',ROOT/'tests/capability_provider_load_test.cpp']
for i,s in enumerate(sources):
 obj=out/f'{i}.o';objects.append(obj)
 subprocess.run(['c++' if s.suffix=='.cpp' else 'cc','-std=c++17' if s.suffix=='.cpp' else '-std=c11',*flags,*['-I'+str(v) for v in incs],'-c',str(s),'-o',str(obj)],check=True)
exe=out/'load-test';subprocess.run(['c++',*flags,*map(str,objects),'-o',str(exe)],check=True)
for size in a.size or [32768,262144,1048576,4194304]:
 result=subprocess.run([str(exe),str(size)]+(['stat-contract'] if a.stat_contract else []),text=True,capture_output=True,timeout=300,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0','UBSAN_OPTIONS':'halt_on_error=1'})
 (out/f'{size}.log').write_text(result.stdout+result.stderr);print(result.stdout+result.stderr,end='');assert result.returncode==0
