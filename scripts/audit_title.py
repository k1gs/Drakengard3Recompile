"""Read-only ELF/layout and HLE registration audit. Registration is not correctness."""
import argparse
import hashlib
import json
import re
import struct
from collections import Counter, defaultdict
from pathlib import Path


def elf_layout(data, base=0):
    ident = data[base:base+16]
    if len(ident) != 16 or ident[:4] != b'\x7fELF' or ident[4] not in (1, 2) or ident[5] not in (1, 2):
        raise ValueError('Invalid ELF identification')
    bits, endian = (64 if ident[4] == 2 else 32), ('>' if ident[5] == 2 else '<')
    fmt = endian + ('HHIQQQIHHHHHH' if bits == 64 else 'HHIIIIIHHHHHH')
    h = struct.unpack_from(fmt, data, base+16)
    segments, sections = [], []
    for i in range(h[9]):
        o = base+h[4]+i*h[8]
        v = struct.unpack_from(endian+('IIQQQQQQ' if bits == 64 else 'IIIIIIII'), data, o)
        typ, flags, off, va, pa, size, mem, align = v if bits == 64 else (v[0],v[6],v[1],v[2],v[3],v[4],v[5],v[7])
        if base+off+size > len(data):
            raise ValueError('Segment outside file')
        segments.append(dict(type=typ, flags=flags, offset=off, vaddr=va, filesz=size, memsz=mem))
    # Embedded ELF section headers may be omitted; only top-level needs them.
    if base == 0:
        for i in range(h[11]):
            v = struct.unpack_from(endian+('IIQQQQIIQQ' if bits == 64 else 'IIIIIIIIII'), data, h[5]+i*h[10])
            sections.append(dict(name_offset=v[0], type=v[1], flags=v[2], address=v[3], offset=v[4], size=v[5], entry_size=v[9]))
        names = sections[h[12]] if sections and h[12] < len(sections) else None
        for s in sections:
            off = names['offset']+s['name_offset'] if names else 0
            end = data.find(b'\0', off)
            s['name'] = data[off:end].decode('ascii', errors='replace') if names and end >= off else ''
    return dict(bits=bits, endian='big' if endian == '>' else 'little', machine=h[1], entry=h[3], segments=segments, sections=sections)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('elf', type=Path)
    p.add_argument('--sdk', type=Path, required=True)
    p.add_argument('--imports', type=Path, required=True)
    p.add_argument('--iso-listing', type=Path, required=True)
    p.add_argument('--out', type=Path, required=True)
    a = p.parse_args()
    if a.out.exists():
        p.error('Choose a new report name')
    data = a.elf.read_bytes()
    layout = elf_layout(data)
    embedded, rejected = [], []
    start = 1
    while True:
        off = data.find(b'\x7fELF', start)
        if off < 0:
            break
        start = off+4
        try:
            e = elf_layout(data, off)
            end = max((s['offset']+s['filesz'] for s in e['segments']), default=0)
            if e['machine'] == 23 and end > 0:
                embedded.append(dict(file_offset=hex(off), mapped_ppu_va=next((hex(s['vaddr']+off-s['offset']) for s in layout['segments'] if s['offset'] <= off < s['offset']+s['filesz']), None),
                                     extent_by_segments=end, sha256=hashlib.sha256(data[off:off+end]).hexdigest(), **e))
        except (ValueError, struct.error) as exc:
            rejected.append(dict(offset=hex(off), error=str(exc)))
    # Use the pinned SDK's own registration scanner, not guesses from import names.
    import sys
    sys.path.insert(0, str(a.sdk/'tools'))
    import gen_hle_nids
    registered = defaultdict(list)
    for module in gen_hle_nids.discover_all():
        files = list((a.sdk/'libs').rglob(module+'.c'))
        if files:
            for nid, name in gen_hle_nids.scan(str(files[0])):
                registered[nid].append(dict(name=name, source=str(files[0].relative_to(a.sdk)), kind='generated-table'))
    # Per-game scaffold overrides are registered separately from the generated table.
    # Static occurrence is a candidate only: build inclusion, call order and conditions matter.
    registration_re = re.compile(r'ps3_hle_register(?:_ctx)?\s*\(\s*(?:(0x[0-9a-fA-F]+)[uU]?|ps3_compute_nid\("([^"]+)"\))\s*,\s*"([^"]+)"\s*,\s*([^;\n]+?)\s*\);')
    for folder in (a.sdk/'runtime', a.sdk/'libs'):
        for file in folder.rglob('*'):
            if file.suffix not in ('.c', '.cpp') or 'tests' in file.parts:
                continue
            source = file.read_text(encoding='latin1')
            source = re.sub(r'/\*.*?\*/|//[^\n]*', lambda m: re.sub(r'[^\n]', ' ', m.group()), source, flags=re.S)
            for match in registration_re.finditer(source):
                literal, computed, name, handler = match.groups()
                nid = int(literal, 16) if literal else gen_hle_nids.compute_nid(computed)
                registered[nid].append(dict(name=name, source=str(file.relative_to(a.sdk)),
                                           line=source.count('\n', 0, match.start())+1,
                                           handler=handler, kind='explicit-registration-occurrence'))
    imports = json.loads(a.imports.read_text(encoding='utf-8'))
    coverage = defaultdict(lambda: dict(total=0, registered=0, missing=[]))
    for row in imports:
        handlers = registered.get(int(row['nid'], 16), [])
        row['registration_candidates'] = handlers
        group = coverage[row['lib']]
        group['total'] += 1
        group['registered'] += bool(handlers)
        if not handlers:
            group['missing'].append(row)
    listing = a.iso_listing.read_text(encoding='utf-8-sig')
    entries = []
    for chunk in listing.split('\n\n'):
        fields = dict(re.findall(r'^([^=\n]+) = (.*)$', chunk, re.M))
        if fields.get('Folder') == '-':
            entries.append(dict(path=fields['Path'], size=int(fields['Size'])))
    counts, sizes = Counter(), Counter()
    for e in entries:
        ext = Path(e['path']).suffix.lower()
        counts[ext] += 1
        sizes[ext] += e['size']
    report = dict(elf_sha256=hashlib.sha256(data).hexdigest(), elf_size=len(data), layout=layout,
                  embedded_spu_candidates=embedded, rejected_elf_signatures=rejected,
                  imports=imports, registration_coverage=dict(coverage),
                  warning='Registration candidates only: no ABI, body semantics or runtime correctness verified.',
                  iso=dict(file_count=len(entries), total_file_bytes=sum(e['size'] for e in entries),
                           extension_counts=dict(counts), extension_bytes=dict(sizes),
                           largest=sorted(entries, key=lambda e:e['size'], reverse=True)[:15]))
    a.out.parent.mkdir(parents=True, exist_ok=True)
    with a.out.open('x', encoding='utf-8') as f:
        json.dump(report, f, indent=2, ensure_ascii=False)
    print(f"ELF {len(data)} bytes, embedded SPU candidates: {len(embedded)}")
    for lib, c in coverage.items():
        print(f"{lib}: {c['registered']}/{c['total']} NIDs have registration candidates")
    print(f"ISO files: {len(entries)}, bytes: {report['iso']['total_file_bytes']}, extensions: {dict(counts)}")


if __name__ == '__main__':
    main()
