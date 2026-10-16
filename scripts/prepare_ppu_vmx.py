"""Replace the guarded vpkswss sites in the local lift with a checked helper."""
import argparse
import re
from pathlib import Path

GUARD = re.compile(rb'port_unimplemented_instruction\(0x[0-9A-F]+ULL, "vpkswss v(\d+), v(\d+), v(\d+)"\);')
DECL = b'extern "C" void port_vpkswss(ppu_context*,unsigned,unsigned,unsigned);\n'


def prepare(directory):
    changed = sites = 0
    for path in directory.glob('ppu_recomp_*.cpp'):
        original = path.read_bytes()
        matches = list(GUARD.finditer(original))
        if not matches:
            sites += original.count(b'port_vpkswss(ctx,')
            continue
        for match in matches:
            if any(int(reg) >= 32 for reg in match.groups()):
                raise ValueError('Invalid VMX register: ' + path.name)
        text = GUARD.sub(lambda m: b'port_vpkswss(ctx,' + b','.join(m.groups()) + b');', original)
        anchor = b'#include "ppu_recomp.h"'
        if text.count(anchor) != 1:
            raise ValueError('Pinned lift include changed: ' + path.name)
        text = text.replace(anchor, anchor + b'\n' + DECL)
        backup = directory/'compat-before-vpkswss'/path.name
        backup.parent.mkdir(exist_ok=True)
        if backup.exists() and backup.read_bytes() != original:
            raise ValueError('Existing VMX backup differs: ' + str(backup))
        if not backup.exists():
            backup.write_bytes(original)
        path.write_bytes(text)
        changed += 1
        sites += len(matches)
    if not sites:
        raise ValueError('No guarded or prepared vpkswss sites found')
    return changed, sites


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--lift', type=Path, required=True)
    args = parser.parse_args()
    chunks, sites = prepare(args.lift)
    print(f'vpkswss: {sites} sites ready, {chunks} chunks changed')
