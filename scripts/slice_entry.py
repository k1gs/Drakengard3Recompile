"""Produce a bounded entry-code slice for exploratory lifting, not a runnable game."""
import argparse
import hashlib
import json
from pathlib import Path
from audit_title import elf_layout

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('elf', type=Path)
p.add_argument('--stage1', type=Path, required=True)
p.add_argument('--out', type=Path, required=True)
a = p.parse_args()
data = a.elf.read_bytes()
info = json.loads((a.stage1/'EBOOT.loader.json').read_text())
entry = int(info['entry_code'], 0)
functions = json.loads((a.stage1/'EBOOT.functions.json').read_text())
function = next(f for f in functions if int(f['start'], 0) == entry)
end = int(function['end'], 0)
if not 0 < end-entry <= 65536:
    raise ValueError('Unexpected entry extent')
seg = next(s for s in elf_layout(data)['segments'] if s['type'] == 1 and s['vaddr'] <= entry < end <= s['vaddr']+s['filesz'])
offset = seg['offset']+entry-seg['vaddr']
a.out.mkdir(parents=True, exist_ok=False)
with (a.out/'entry.raw').open('xb') as f:
    f.write(data[offset:offset+end-entry])
with (a.out/'functions.json').open('x') as f:
    json.dump([function], f, indent=2)
with (a.out/'provenance.json').open('x') as f:
    json.dump(dict(elf_sha256=hashlib.sha256(data).hexdigest(), file_offset=hex(offset),
                   entry=info['entry_code'], end=hex(end), toc=info['entry_toc'],
                   caveat='Extent from OPD ordering; validate control flow before executing.'), f, indent=2)
print(f"Entry slice {hex(entry)}..{hex(end)}, {end-entry} bytes, file offset {hex(offset)}")
