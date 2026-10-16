"""Lift this title's MultiStream DSP plugins at their observed runtime LS addresses.

Input bytes and all generated files remain local. No game code is embedded here.
The layout was verified against the run092 LS capture; other EBOOTs fail closed.
"""
import argparse
import hashlib
import struct
import subprocess
import sys
from pathlib import Path

EBOOT_SHA256 = '454fb591e9192cf9d4a5c65912eb4cd8c982ba52e26474bcfc9dc6ee1dc12acc'
MODULES = ((0x18F8680, 0x37000, 0x1F80),
           (0x18FCD80, 0x39000, 0x890),
           (0x1905A00, 0x39900, 0x420))


def extract(data):
    if hashlib.sha256(data).hexdigest() != EBOOT_SHA256:
        raise ValueError('Unsupported EBOOT; verify plugin layout before lifting')
    block = bytearray(0x2D20)
    seeds = set()
    spans = []
    for at, base, expected_size in MODULES:
        elf = data[at:]
        if elf[:6] != b'\x7fELF\x01\x02' or struct.unpack_from('>H', elf, 18)[0] != 23:
            raise ValueError('Expected embedded BE SPU ELF')
        entry, phoff = struct.unpack_from('>II', elf, 24)
        phnum = struct.unpack_from('>H', elf, 44)[0]
        segments = [struct.unpack_from('>8I', elf, phoff + i * 32) for i in range(phnum)]
        code = [s for s in segments if s[0] == 1 and s[6] & 1]
        if len(code) != 1 or code[0][4] != expected_size or code[0][2] != 0x80:
            raise ValueError('Plugin segment layout changed')
        _, offset, va, _, size, _, _, _ = code[0]
        text = elf[offset:offset + size]
        block[base - 0x37000:base - 0x37000 + size] = text
        spans.append((base, size))
        seeds.update((base + entry - va, base + 0x10))
        words = struct.unpack(f'>{size // 4}I', text)
        # Callback entries follow returns; internal references alone miss them.
        for i, word in enumerate(words):
            if word != 0x35000000:  # bi r0
                continue
            j = i + 1
            while j < len(words) and words[j] in (0x4020007F, 0x00200000):
                j += 1
            if j < len(words) and words[j]:
                seeds.add(base + j * 4)
    return bytes(block), sorted(seeds), spans


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('eboot', type=Path)
    p.add_argument('--sdk', type=Path, required=True)
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--snapshot', type=Path)
    a = p.parse_args()
    block, seeds, spans = extract(a.eboot.read_bytes())
    if a.snapshot:
        ls = a.snapshot.read_bytes()
        if len(ls) != 0x40000 or any(ls[b:b+n] != block[b-0x37000:b-0x37000+n] for b, n in spans):
            p.error('Runtime code does not match the extracted plugins')
    if a.out.exists():
        p.error('Choose a fresh output directory')
    a.out.mkdir(parents=True)
    raw, elf = a.out / 'plugins.bin', a.out / 'plugins.elf'
    raw.write_bytes(block)
    subprocess.run([sys.executable, str(a.sdk/'tools/wrap_spu_elf.py'), str(raw),
                    '--base', '0x37000', '--entry', hex(seeds[0]), '--out', str(elf)], check=True)
    subprocess.run([sys.executable, str(a.sdk/'tools/spu_lifter.py'), '--auto-functions', str(elf),
                    '--extra-funcs', ','.join(hex(s) for s in seeds), '--symbol-prefix', 'port_msdsp_',
                    '-o', str(a.out)], check=True)
    for name in ('spu_recomp.c', 'spu_recomp.h'):
        path = a.out/name
        text = path.read_text(encoding='utf-8')
        for header in ('spu_context.h', 'spu_helpers.h'):
            text = text.replace(f'"../../runtime/spu/{header}"', f'"{header}"')
        if '/* TODO spu:' in text:
            raise ValueError('DSP lift contains unsupported instructions')
        path.write_text(text, encoding='utf-8')
    rows = ['/* Generated from user-supplied EBOOT; never publish. */',
            'static const unsigned char port_msdsp_bytes[] = {']
    rows += ['    '+','.join(f'0x{x:02X}' for x in block[i:i+16])+',' for i in range(0,len(block),16)]
    rows += ['};', 'extern void port_msdsp_spu_recomp_register(void);']
    (a.out/'msdsp_bytes.h').write_text('\n'.join(rows)+'\n', encoding='utf-8')
    print(f'DSP lift: {len(block)} bytes, {len(seeds)} callback seeds, snapshot verified={bool(a.snapshot)}')


if __name__ == '__main__':
    main()
