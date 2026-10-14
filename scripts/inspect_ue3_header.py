"""Inspect only the common UE3 package prefix; no decompression or object parsing."""
import argparse
import hashlib
import json
import struct
from pathlib import Path


def parse_prefix(data, file_size):
    if data[:4] == b'\x9e\x2a\x83\xc1':
        endian = '>'
    elif data[:4] == b'\xc1\x83\x2a\x9e':
        endian = '<'
    else:
        raise ValueError('No standard Unreal package tag')
    version, header_size, length = struct.unpack_from(endian+'IIi', data, 4)
    name_bytes = length if length >= 0 else -length*2
    if name_bytes > 4096 or 16+name_bytes+28 > len(data):
        raise ValueError('Folder FString outside bounded prefix')
    raw = data[16:16+name_bytes]
    folder = raw.decode('utf-16-be' if endian == '>' else 'utf-16-le', errors='replace') if length < 0 else raw.decode('ascii', errors='replace')
    flags, names, name_offset, exports, export_offset, imports, import_offset = struct.unpack_from(endian+'7I', data, 16+name_bytes)
    if max(header_size, name_offset, export_offset, import_offset) > file_size:
        raise ValueError('Package tables outside file')
    return dict(endian='big' if endian == '>' else 'little', file_version=version & 0xffff,
                licensee_version=version >> 16, header_size=header_size, folder=folder.rstrip('\0'),
                flags=hex(flags), names=names, name_offset=name_offset,
                exports=exports, export_offset=export_offset, imports=imports, import_offset=import_offset,
                caveat='Common UE3 prefix only; table contents and object serialization unvalidated.')


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('package', type=Path)
    p.add_argument('--out', type=Path, required=True)
    a = p.parse_args()
    if a.out.exists():
        p.error('Choose a fresh report path')
    with a.package.open('rb') as f:
        prefix = f.read(8192)
        f.seek(0)
        digest = hashlib.sha256()
        for chunk in iter(lambda:f.read(1024*1024), b''):
            digest.update(chunk)
    result = parse_prefix(prefix, a.package.stat().st_size)
    result.update(path=str(a.package), sha256=digest.hexdigest())
    with a.out.open('x', encoding='utf-8') as f:
        json.dump(result, f, indent=2)
    print(json.dumps(result, indent=2))
