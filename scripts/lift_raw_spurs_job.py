"""Lift a captured raw SPURS job with an explicit LS base/entry, not WWS defaults."""
import argparse
import hashlib
import json
import re
import subprocess
import sys
from pathlib import Path


def fingerprint(data):
    # Pinned SDK uses this nonstandard FNV offset basis, not FNV-1a's usual one.
    value=1469598103934665603
    for byte in data:
        value=((value^byte)*0x100000001b3)&0xffffffffffffffff
    return value


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('input',type=Path)
    p.add_argument('--sdk',type=Path,required=True)
    p.add_argument('--out',type=Path,required=True)
    p.add_argument('--base',type=lambda v:int(v,0),required=True)
    p.add_argument('--entry',type=lambda v:int(v,0),required=True)
    p.add_argument('--image-id',type=int,required=True)
    a=p.parse_args()
    raw=a.input.read_bytes(); fp=fingerprint(raw); name=f'rawjob_{fp:016X}'; prefix=f'raw_{fp:016X}_'
    captured=re.fullmatch(r'spujob_([0-9A-Fa-f]{16})_([0-9]+)\.bin',a.input.name)
    if captured and (int(captured[1],16)!=fp or int(captured[2])!=len(raw)):
        p.error('Capture filename fingerprint/size does not match the pinned runtime algorithm')
    if not raw or len(raw)%16 or not 0<=a.base<=a.entry<a.base+len(raw)<=0x40000:
        p.error('Invalid raw LS image/base/entry')
    manifest=a.out/'raw_jobs_manifest.json'
    rows=json.loads(manifest.read_text()) if manifest.exists() else []
    if any(row['fingerprint']==f'{fp:016X}' or row['image_id']==a.image_id for row in rows):
        p.error('Fingerprint/image ID already registered; preserve existing lift')
    directory=a.out/name
    if directory.exists(): p.error('Choose a fresh raw image output')
    directory.mkdir(parents=True)
    elf=directory/'wrapped.elf'
    subprocess.run([sys.executable,str(a.sdk/'tools/wrap_spu_elf.py'),str(a.input),'--base',hex(a.base),'--entry',hex(a.entry),'--out',str(elf)],check=True)
    subprocess.run([sys.executable,str(a.sdk/'tools/spu_lifter.py'),'--auto-functions',str(elf),'--symbol-prefix',prefix,'-o',str(directory)],check=True)
    source=directory/'spu_recomp.c'
    for path in (source,directory/'spu_recomp.h'):
        text=path.read_text(encoding='utf-8')
        for header in ('spu_context.h','spu_helpers.h'):
            text=text.replace(f'"../../runtime/spu/{header}"',f'"{header}"')
        path.write_text(text,encoding='utf-8')
    text=source.read_text(encoding='utf-8')
    entry=f'{prefix}spu_func_{a.entry:08X}'
    if f'void {entry}(' not in text:
        raise ValueError('Explicit entry not emitted by lifter')
    rows.append(dict(name=name,prefix=prefix,entry=entry,image_id=a.image_id,fingerprint=f'{fp:016X}',
                     size=len(raw),sha256=hashlib.sha256(raw).hexdigest(),base=a.base,entry_ls=a.entry,
                     unsupported_sites=text.count('/* TODO spu:')))
    lines=['/* Generated from local raw job captures. Never publish this file. */','#include "spu_workload.h"','extern void spu_begin_image(int);']
    for row in rows:
        lines += [f'extern void {row["entry"]}(spu_context*);',f'extern void {row["prefix"]}spu_recomp_register(void);']
    lines += ['void drakengard_raw_jobs_register_all(void) {']
    for row in rows:
        lines += [f'    spu_begin_image({row["image_id"]});',f'    {row["prefix"]}spu_recomp_register();',
                  f'    spu_workload_register_img(0x{row["fingerprint"]}ULL, {row["entry"]}, {row["image_id"]}, "{row["name"]}");']
    lines += ['}']
    (a.out/'raw_jobs_register.c').write_text('\n'.join(lines)+'\n',encoding='utf-8')
    manifest.write_text(json.dumps(rows,indent=2),encoding='utf-8')
    print(f'Registered raw job {fp:016X}, {len(raw)} bytes, image ID {a.image_id}')


if __name__=='__main__':
    main()
