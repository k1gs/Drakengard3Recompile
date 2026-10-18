"""Lift the observed decoder from verified local SPU captures; never embed game bytes."""
import argparse
import hashlib
import struct
import subprocess
import sys
from pathlib import Path

RESIDENT_SHA = 'e33837bb55e56b6d6ae06bac574b8fa614976a6c4cf40b0b98348c868d490c53'
BASE, END, ENTRY = 0x1A900, 0x24000, 0x21D20


def array(name, data):
    rows = [f'static const unsigned char {name}[] = {{']
    rows += ['    '+','.join(f'0x{x:02X}' for x in data[i:i+16])+',' for i in range(0,len(data),16)]
    return '\n'.join(rows+['};'])+'\n'


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('capture', type=Path)
    p.add_argument('--reference', type=Path, required=True)
    p.add_argument('--resident', type=Path, required=True)
    p.add_argument('--sdk', type=Path, required=True)
    p.add_argument('--out', type=Path, required=True)
    a = p.parse_args()
    capture = a.capture.read_bytes()
    if len(capture)!=16+2048+0x40000 or struct.unpack_from('<4I',capture)!=(0x41554431,41,ENTRY,0):
        p.error('Expected the complete AUD1 image-41 decoder entry capture')
    ls = capture[2064:]
    resident = a.resident.read_bytes()
    if hashlib.sha256(resident).hexdigest()!=RESIDENT_SHA or ls[0x3000:0x1A3F0]!=resident[0x100:0x174F0]:
        p.error('Resident program differs from the verified image-41 owner')
    reference = a.reference.read_bytes()
    if len(reference)!=0x40000 or reference[BASE:END]!=ls[BASE:END]:
        p.error('Decoder code differs across the independent captures')
    if a.out.exists():
        p.error('Choose a fresh output directory')
    a.out.mkdir(parents=True)
    raw, elf = a.out/'decoder.bin', a.out/'decoder.elf'
    raw.write_bytes(ls[BASE:END])
    subprocess.run([sys.executable,str(a.sdk/'tools/wrap_spu_elf.py'),str(raw),'--base',hex(BASE),
                    '--entry',hex(ENTRY),'--out',str(elf)],check=True)
    subprocess.run([sys.executable,str(a.sdk/'tools/spu_lifter.py'),'--auto-functions',str(elf),
                    '--symbol-prefix','port_audio_','-o',str(a.out)],check=True)
    for name in ('spu_recomp.c','spu_recomp.h'):
        path=a.out/name
        text=path.read_text(encoding='utf-8')
        for header in ('spu_context.h','spu_helpers.h'):
            text=text.replace(f'"../../runtime/spu/{header}"',f'"{header}"')
        if '/* TODO spu:' in text:
            raise ValueError('Unsupported decoder instructions')
        path.write_text(text,encoding='utf-8')
    (a.out/'audio_decoder_bytes.h').write_text(f'#define PORT_AUDIO_BASE 0x{BASE:X}u\n#define PORT_AUDIO_END 0x{END:X}u\n'+array('port_audio_bytes',ls[BASE:END])+
        'extern void port_audio_spu_recomp_register(void);\n',encoding='utf-8')
    (a.out/'audio_decoder_fixture.h').write_text(array('port_audio_input',capture[16:]),encoding='utf-8')
    print('Decoder: owner verified; complete code matches independent capture; real replay inputs retained locally')


if __name__=='__main__':
    main()
