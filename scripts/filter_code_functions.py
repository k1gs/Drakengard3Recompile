"""Filter heuristic function ranges by ELF SHF_EXECINSTR; not semantic validation."""
import argparse
import hashlib
import json
from pathlib import Path
from audit_title import elf_layout

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('elf', type=Path)
p.add_argument('--functions', type=Path, required=True)
p.add_argument('--out', type=Path, required=True)
a = p.parse_args()
data = a.elf.read_bytes()
ranges = [(s['address'],s['address']+s['size']) for s in elf_layout(data)['sections'] if s['flags'] & 4 and s['size']]
if not ranges:
    raise ValueError('No SHF_EXECINSTR sections; manual map required')
accepted, rejected = [], []
for f in json.loads(a.functions.read_text()):
    start, end = int(f['start'], 0), int(f['end'], 0)
    if any(lo <= start < end <= hi for lo, hi in ranges):
        accepted.append(f)
    else:
        rejected.append(f)
a.out.mkdir(parents=True, exist_ok=False)
for name, obj in [('functions.json', accepted), ('rejected.json', rejected),
                  ('provenance.json', dict(elf_sha256=hashlib.sha256(data).hexdigest(),
                                          code_ranges=[[hex(lo),hex(hi)] for lo,hi in ranges],
                                          accepted=len(accepted),rejected=len(rejected),
                                          caveat='ELF executable-section containment only; boundaries and semantics still unvalidated.'))]:
    with (a.out/name).open('x', encoding='utf-8') as f:
        json.dump(obj, f, indent=2)
print(f'{len(accepted)} ranges contained in executable sections; {len(rejected)} require review')
