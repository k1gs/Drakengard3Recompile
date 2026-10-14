"""Read-only intake. Run only after the downloader reports completion.

No extraction, decryption, code execution or writes under the input directory.
"""
import argparse
import hashlib
import json
import struct
from collections import Counter
from datetime import datetime, timezone
from pathlib import Path


def parse_sfo(data):
    if len(data) < 20 or data[:4] != b'\x00PSF':
        raise ValueError('Not a PARAM.SFO')
    _, version, keys, values, count = struct.unpack_from('<5I', data)
    if count > 4096 or 20 + count * 16 > len(data):
        raise ValueError('Invalid SFO index')
    result = {}
    for i in range(count):
        key, fmt, size, capacity, offset = struct.unpack_from('<HHIII', data, 20 + i * 16)
        begin = keys + key
        end = data.find(b'\x00', begin)
        start = values + offset
        if begin >= len(data) or end < 0 or start + size > len(data) or size > capacity:
            raise ValueError('Invalid SFO entry bounds')
        name = data[begin:end].decode('utf-8', errors='replace')
        raw = data[start:start + size]
        result[name] = (struct.unpack('<I', raw)[0] if fmt == 0x0404 and size == 4
                        else raw.rstrip(b'\x00').decode('utf-8', errors='replace')
                        if fmt == 0x0204 else raw.hex())
    return {'version': version, 'values': result}


def parse_elf(data, file_size):
    if data[:4] != b'\x7fELF':
        raise ValueError('Not an ELF')
    if len(data) < 16 or data[4] not in (1, 2) or data[5] not in (1, 2):
        raise ValueError('Invalid ELF identification')
    endian = '<' if data[5] == 1 else '>'
    bits = 64 if data[4] == 2 else 32
    fmt = endian + ('HHIQQQIHHHHHH' if bits == 64 else 'HHIIIIIHHHHHH')
    if len(data) < 16 + struct.calcsize(fmt):
        raise ValueError('Truncated ELF header')
    h = struct.unpack_from(fmt, data, 16)
    return {'bits': bits, 'endian': 'little' if endian == '<' else 'big',
            'machine': h[1], 'machine_name': {21: 'PowerPC64/PPU', 23: 'SPU', 20: 'PowerPC'}.get(h[1], 'other'),
            'type': h[0], 'entry': hex(h[3]), 'program_headers_offset': h[4],
            'program_headers_count': h[9], 'section_headers_count': h[11],
            'program_headers_in_file': h[4] + h[8] * h[9] <= file_size}


def inspect(path, base, hash_all=False):
    before = path.stat()
    row = {'path': str(path.relative_to(base)), 'size': before.st_size,
           'mtime_ns': before.st_mtime_ns}
    with path.open('rb') as f:
        head = f.read(64)
        row['magic_hex'] = head[:16].hex()
        if head[:4] == b'\x7fELF':
            row['elf'] = parse_elf(head, before.st_size)
        elif head[:4] == b'SCE\x00':
            row['container'] = 'SELF/SCE; plain ELF needed for static analysis'
        elif head[:4] == b'\x7fPKG':
            row['container'] = 'PS3 PKG'
        elif head[:4] == b'\x00PSF' and before.st_size <= 1024 * 1024:
            f.seek(0)
            row['sfo'] = parse_sfo(f.read())
        if hash_all or path.name.upper() in ('EBOOT.BIN', 'PARAM.SFO') or path.suffix.lower() in ('.elf', '.self', '.sprx'):
            f.seek(0)
            digest = hashlib.sha256()
            for chunk in iter(lambda: f.read(4 * 1024 * 1024), b''):
                digest.update(chunk)
            row['sha256'] = digest.hexdigest()
    after = path.stat()
    if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
        raise ValueError(f'Input changed during read: {path}')
    return row


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('input', type=Path)
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--download-complete', action='store_true', help='Explicitly confirm downloader completion')
    p.add_argument('--hash-all', action='store_true', help='Stream SHA-256 for all files, including large images')
    args = p.parse_args()
    if not args.download_complete:
        p.error('Wait for downloader completion, then supply --download-complete')
    source, output = args.input.resolve(), args.out.resolve()
    base = source if source.is_dir() else source.parent
    if source.is_dir() and output.is_relative_to(source) or output == source:
        p.error('Report must be outside input')
    if output.exists():
        p.error('Report already exists; choose a new name')
    rows, errors = [], []
    paths = sorted(source.rglob('*')) if source.is_dir() else [source]
    for path in paths:
        if path.is_symlink() or not path.is_file():
            continue
        try:
            rows.append(inspect(path, base, args.hash_all))
        except (OSError, ValueError, struct.error) as exc:
            errors.append({'path': str(path), 'error': str(exc)})
    report = {'captured_utc': datetime.now(timezone.utc).isoformat(), 'input': str(source),
              'hash_all': args.hash_all, 'files': rows, 'errors': errors,
              'extensions': dict(Counter(Path(r['path']).suffix.lower() for r in rows))}
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open('x', encoding='utf-8') as f:
        json.dump(report, f, indent=2, ensure_ascii=False)
    print(f'{len(rows)} files, {len(errors)} errors -> {output}')
    return 1 if errors or not rows else 0


if __name__ == '__main__':
    raise SystemExit(main())
