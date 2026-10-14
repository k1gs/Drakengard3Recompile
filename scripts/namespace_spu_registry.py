"""Move title SPU IDs out of the SDK's legacy hardcoded title-adapter range."""
import argparse
import re
from pathlib import Path


def namespace(text, offset):
    ids={int(v) for v in re.findall(r'spu_begin_image\((\d+)\)',text)}
    if ids != set(range(0,41)) or offset<40 or offset+40>=100:
        raise ValueError('Expected 40 original IDs and a namespace below raw-job IDs 100/101')
    text=re.sub(r'spu_begin_image\((\d+)\)',lambda m:f'spu_begin_image({int(m[1])+offset if int(m[1]) else 0})',text)
    pattern=r'(spu_workload_register_img\(0x[0-9A-Fa-f]+ULL,\s*\w+,\s*)(\d+)(,\s*"[^"]+"\);)'
    def replace(m):
        old=int(m[2])
        if old not in ids or old == 0: raise ValueError('Registration ID not in image declaration set')
        return m[1]+str(old+offset)+m[3]
    text,count=re.subn(pattern,replace,text)
    if count<40: raise ValueError('Missing workload registration sites')
    return text


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('input',type=Path)
    p.add_argument('--out',type=Path,required=True)
    p.add_argument('--offset',type=int,default=40)
    a=p.parse_args()
    text=namespace(a.input.read_text(encoding='utf-8'),a.offset)
    if not a.out.exists() or a.out.read_text(encoding='utf-8')!=text:
        a.out.write_text(text,encoding='utf-8')
    print('Title SPU ELF IDs: 41..80; raw job IDs unchanged')


if __name__=='__main__': main()
