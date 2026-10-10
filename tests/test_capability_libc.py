#!/usr/bin/env python3
import os,subprocess
from pathlib import Path
root=Path(__file__).resolve().parents[1]
out=root/'build/capability-libc';out.mkdir(parents=True,exist_ok=True)
for mode,flags in [('normal',[]),('sanitized',['-fsanitize=address,undefined','-fno-omit-frame-pointer','-fno-pie','-no-pie'])]:
 obj=out/(mode+'.o');exe=out/mode
 subprocess.run(['cc','-std=c11','-D_POSIX_C_SOURCE=200809L','-O1','-g',*flags,'-Dvsnprintf=cap_test_vsnprintf','-Dstrtok_r=cap_test_strtok_r','-Dtoupper=cap_test_toupper','-c',str(root/'capability/libc_compat.c'),'-o',str(obj)],check=True)
 subprocess.run(['cc','-std=c11','-D_POSIX_C_SOURCE=200809L','-O1','-g',*flags,str(root/'tests/capability_libc_test.c'),str(obj),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0:halt_on_error=1','UBSAN_OPTIONS':'halt_on_error=1'})
