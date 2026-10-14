"""Inspect the constructor-dispatch table observed in the pinned PPU build."""
import argparse
import hashlib
import json
import struct
from pathlib import Path
from audit_title import elf_layout
from generate_startup_probe import EXPECTED_SHA, TOC


def walk_ctor_table(read_u32, end, limit=4096):
    rows = []
    for i in range(limit):
        slot = end-4*(i+1)
        opd = read_u32(slot)
        if opd == 0xffffffff:
            return rows, slot
        if opd == 0:
            raise ValueError('Null OPD before constructor sentinel')
        rows.append(dict(order=i, slot=hex(slot), opd=hex(opd), code=hex(read_u32(opd)), toc=hex(read_u32(opd+4))))
    raise ValueError('Constructor sentinel not found within bounded walk')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--elf', type=Path, default=Path('work/extracted/PS3_GAME/USRDIR/EBOOT.elf'))
    p.add_argument('--functions', type=Path, default=Path('work/ppu-section-filtered-001/functions.json'))
    p.add_argument('--out', type=Path, default=Path('reports/constructor-audit-001.json'))
    a = p.parse_args()
    if a.out.exists():
        p.error('Choose a fresh output path')
    data = a.elf.read_bytes()
    if hashlib.sha256(data).hexdigest()!=EXPECTED_SHA:
        p.error('Executable revision differs from the observed build')
    layout = elf_layout(data)
    segments = [s for s in layout['segments'] if s['type']==1]
    def read_u32(address):
        for s in segments:
            if s['vaddr'] <= address < address+4 <= s['vaddr']+s['filesz']:
                return struct.unpack_from('>I',data,s['offset']+address-s['vaddr'])[0]
        raise ValueError(f'Address outside file-backed segments: {address:#x}')
    end = read_u32(TOC-0x7fb0)
    rows, sentinel = walk_ctor_table(read_u32,end)
    functions = {int(row['start'],0):row for row in json.loads(a.functions.read_text())}
    code_ranges = [(s['address'],s['address']+s['size']) for s in layout['sections'] if s['flags']&4]
    for row in rows:
        code = int(row['code'],0)
        f = functions.get(code)
        row.update(in_executable_section=any(lo<=code<hi for lo,hi in code_ranges),
                   boundary_found=f is not None,
                   function_bytes=int(f['end'],0)-code if f else None)
    valid = all(row['in_executable_section'] and row['boundary_found'] and int(row['toc'],0)==TOC for row in rows)
    report = dict(elf_sha256=EXPECTED_SHA, dispatcher='0xa8a408', toc_slot=hex(TOC-0x7fb0),
                  table_end=hex(end), sentinel_slot=hex(sentinel), entries=rows,
                  unique_codes=len({row['code'] for row in rows}), all_entries_mapped_with_primary_toc=valid,
                  total_candidate_function_bytes=sum(row['function_bytes'] or 0 for row in rows),
                  optional_init_slots={hex(TOC+x):hex(read_u32(TOC+x)) for x in (-0x7fc4,-0x7fc0,-0x7fbc,-0x7fb8,-0x7fb4)},
                  caveat='Table order/OPD mapping only. Callees, runtime effects and execution unvalidated.')
    a.out.parent.mkdir(parents=True,exist_ok=True)
    a.out.write_text(json.dumps(report,indent=2),encoding='utf-8')
    print(f'{len(rows)} initialization entries, {report["unique_codes"]} unique code targets, {report["total_candidate_function_bytes"]} candidate code bytes, mapping_valid={valid}')
    return 0 if valid else 1


if __name__ == '__main__':
    raise SystemExit(main())
