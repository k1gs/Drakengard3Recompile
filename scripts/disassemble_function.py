"""Bounded PPU disassembly; produces local evidence, not recovered source."""
import argparse
import hashlib
import json
import struct
import sys
from pathlib import Path
from audit_title import elf_layout

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('elf', type=Path)
p.add_argument('--sdk', type=Path, required=True)
p.add_argument('--functions', type=Path, required=True)
p.add_argument('--imports', type=Path, required=True)
p.add_argument('--address', type=lambda s:int(s, 0), required=True)
p.add_argument('--out', type=Path, required=True)
a = p.parse_args()
if a.out.exists():
    p.error('Choose a fresh report path')
data = a.elf.read_bytes()
functions = json.loads(a.functions.read_text())
fn = next(f for f in functions if int(f['start'], 0) == a.address)
end = int(fn['end'], 0)
if not 0 < end-a.address <= 65536:
    raise ValueError('Function too large for bounded exploration')
seg = next(s for s in elf_layout(data)['segments'] if s['type'] == 1 and s['vaddr'] <= a.address < end <= s['vaddr']+s['filesz'])
offset = seg['offset']+a.address-seg['vaddr']
imports = {int(i['stub'], 0):i for i in json.loads(a.imports.read_text())}
sys.path.insert(0, str(a.sdk/'tools'))
from ppu_disasm import decode
lines = [f"ELF SHA256 {hashlib.sha256(data).hexdigest()}", f"VA {hex(a.address)}..{hex(end)} file offset {hex(offset)}", 'Boundaries are heuristic; direct calls alone are not a full call graph.']
for i in range(0, end-a.address, 4):
    word = struct.unpack_from('>I', data, offset+i)[0]
    ins = decode(word, a.address+i)
    note = ''
    if word >> 26 == 18:
        displacement = word & 0x03fffffc
        if displacement & 0x02000000:
            displacement -= 0x04000000
        target = (displacement if word & 2 else a.address+i+displacement) & 0xffffffff
        imp = imports.get(target)
        note = f" -> {imp['lib']}::{imp.get('name', imp['nid'])}" if imp else f' -> VA {hex(target)}'
    lines.append(f'{a.address+i:08x} {word:08x} {ins.mnemonic} {ins.operands}{note}')
a.out.parent.mkdir(parents=True, exist_ok=True)
with a.out.open('x', encoding='utf-8') as f:
    f.write('\n'.join(lines)+'\n')
print(f'{len(lines)-3} instructions -> {a.out}')
