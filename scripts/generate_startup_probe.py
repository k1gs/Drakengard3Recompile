"""Lift observed startup ranges with explicit stop barriers at the frontier.

Generated game code stays under work/. No imports are implemented or faked here.
"""
import argparse
import hashlib
import json
import re
import sys
from pathlib import Path
from audit_title import elf_layout

EXPECTED_SHA = '454fb591e9192cf9d4a5c65912eb4cd8c982ba52e26474bcfc9dc6ee1dc12acc'
ENTRY, CRT, TOC = 0xa8a020, 0xa8a128, 0x1981090


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--elf', type=Path, default=Path('work/extracted/PS3_GAME/USRDIR/EBOOT.elf'))
    p.add_argument('--sdk', type=Path, default=Path('third_party/ps3recomp'))
    p.add_argument('--functions', type=Path, default=Path('work/ppu-section-filtered-001/functions.json'))
    p.add_argument('--imports', type=Path, default=Path('work/ppu-stage1/imports_named.json'))
    p.add_argument('--out', type=Path, default=Path('work/startup-probe-005'))
    a = p.parse_args()
    data = a.elf.read_bytes()
    if hashlib.sha256(data).hexdigest() != EXPECTED_SHA:
        p.error('Executable revision differs from the observed build')
    if a.out.exists():
        p.error('Choose a fresh output directory')
    layout = elf_layout(data)
    tls = next(s for s in layout['segments'] if s['type']==7)
    functions = {int(f['start'], 0): f for f in json.loads(a.functions.read_text())}
    imports = {int(f['stub'], 0): f for f in json.loads(a.imports.read_text())}
    sys.path.insert(0, str(a.sdk/'tools'))
    from ppu_lifter import PPULifter
    from ppu_disasm import disassemble_bytes
    lifter = PPULifter()
    # The window spans actual executable sections, not the limited instruction slice.
    # Outside selected functions, referenced targets get explicit throwing barriers.
    code = [s for s in layout['sections'] if s['flags'] & 4]
    lifter.code_lo = min(s['address'] for s in code)
    lifter.code_hi = max(s['address']+s['size'] for s in code)
    lifter.toc_base = TOC
    # The filtered map splits the entry return/exit continuation at 0xa8a044.
    # Include that range even though the intended first-import stop precedes it.
    starts = (ENTRY, 0xa8a044, CRT, 0xea2ee4)
    lifter.function_entries = set(starts)
    lifter.hle_stub_nids = {addr:int(row['nid'], 0) for addr,row in imports.items()}
    selected = []
    for start in starts:
        end = int(functions[start]['end'], 0)
        segment = next(s for s in layout['segments'] if s['type'] == 1 and s['vaddr'] <= start < end <= s['vaddr']+s['filesz'])
        off = segment['offset']+start-segment['vaddr']
        instructions = disassemble_bytes(data[off:off+end-start], start, True)
        f = lifter.lift_function(instructions, start, end)
        selected.append(dict(start=hex(start), end=hex(end), instructions=len(instructions), file_offset=hex(off)))
        bad = [line for line in f.body_lines if re.search(r'TODO|non-code|undecoded|unmodeled|unknown', line, re.I)]
        if bad:
            raise ValueError(f'Unverified lowering in {hex(start)}: {bad}')
    referenced = lifter.call_targets | lifter.branch_targets
    imported = sorted(referenced & imports.keys())
    for address in imported:
        lifter.lift_function([], address, address+4)
    defined = {f.start_addr for f in lifter.functions}
    frontier = sorted((lifter.call_targets | lifter.branch_targets)-defined)
    source = lifter.emit_source()
    if re.search(r'non-code|missing continuation|branch into undecoded', source):
        raise ValueError('Generated source contains a silently discarded path')
    a.out.mkdir(parents=True)
    (a.out/'ppu_recomp.h').write_text(lifter.emit_header(), encoding='utf-8')
    (a.out/'ppu_recomp.cpp').write_text(source, encoding='utf-8')
    barriers = ['#include "ppu_recomp.h"', '[[noreturn]] void probe_frontier(unsigned long long target);']
    for address in frontier:
        barriers.append(f'void func_{address:08X}(ppu_context*) {{ probe_frontier(0x{address:X}ULL); }}')
    (a.out/'frontier.cpp').write_text('\n'.join(barriers)+'\n', encoding='utf-8')
    (a.out/'probe_config.h').write_text(f'#pragma once\n#define PROBE_ELF_SHA "{EXPECTED_SHA}"\n#define PROBE_ELF_SIZE {len(data)}ULL\n#define PROBE_ENTRY 0x{ENTRY:X}ULL\n#define PROBE_TOC 0x{TOC:X}ULL\n#define PROBE_ENTRY_OPD 0x196A428ULL\n#define PROBE_TLS_VADDR 0x{tls["vaddr"]:X}ULL\n#define PROBE_TLS_FILESZ {tls["filesz"]}ULL\n#define PROBE_TLS_MEMSZ {tls["memsz"]}ULL\n', encoding='utf-8')
    report = dict(elf_sha256=EXPECTED_SHA,
                  tool_sha256={name:hashlib.sha256((a.sdk/'tools'/name).read_bytes()).hexdigest() for name in ('ppu_lifter.py','ppu_disasm.py')},
                  selected=selected, tls=tls, imported_stops=[imports[x] for x in imported],
                  explicit_frontier_stops=[hex(x) for x in frontier],
                  intended_stop=dict(nid='0x744680a2', name='sys_initialize_tls', lr='0xa8a180'),
                  caveat='Synthetic boot registers. No HLE implementation. Must stop at first import; no game execution beyond it.')
    (a.out/'provenance.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    print(f'Lifted {len(selected)} guest ranges, {len(imported)} import stops, {len(frontier)} frontier barriers -> {a.out}')


if __name__ == '__main__':
    main()
