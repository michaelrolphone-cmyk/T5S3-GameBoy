#!/usr/bin/env python3
import argparse,os,subprocess,sys,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];sys.path.insert(0,str(ROOT/'minimal'))
import build
p=argparse.ArgumentParser();p.add_argument('--runtime',type=Path,required=True);p.add_argument('--sanitize',action='store_true');a=p.parse_args()
san=['-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-omit-frame-pointer'] if a.sanitize else []
with tempfile.TemporaryDirectory() as tmp:
    temp=Path(tmp);flags=['cc','-std=gnu11','-O1','-g',*san,*build.includes(a.runtime,temp/'source')]
    model=temp/'model'
    subprocess.run([*flags,str(ROOT/'minimal/model.c'),str(ROOT/'minimal/integer.c'),str(ROOT/'tests/minimal_model_test.c'),'-o',str(model)],check=True);subprocess.run([str(model)],check=True)
    src=build.sources(temp/'source');objects=[]
    for i,source in enumerate(src+[ROOT/'tests/minimal_app_test.c']):
        obj=temp/f'{i}.o';objects.append(obj)
        subprocess.run([*flags,'-DGBEMU_FAST_MONO=1','-ffunction-sections','-fdata-sections','-c',str(source),'-o',str(obj)],check=True)
    app=temp/'app';subprocess.run([*flags,'-Wl,--gc-sections','-Wl,--wrap=malloc,--wrap=calloc',*map(str,objects),'-o',str(app)],check=True)
    for mode in ['chooser','receiver','landscape','paper','bad-rom','cgb-only','short-read','error-read','size-limit','cancel','close-retained','present-timeout','present-failed','grant-retained','old-runtime','bad-display','surface-overflow','missing-grant','unavailable-media','invalid-source','directory-limit','init-oom','rom-oom','core-oom']:
        subprocess.run([str(app),mode],check=True,timeout=30)
