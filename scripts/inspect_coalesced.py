"""Validate UE3 coalesced config: BE counts, explicit UTF-16 string byte order."""
import argparse
import hashlib
import json
import struct
from pathlib import Path


class Reader:
    def __init__(self, data, wide_endian='little'):
        self.data, self.offset = data, 0
        self.wide_endian=wide_endian

    def take(self, size):
        if size < 0 or size > len(self.data)-self.offset:
            raise ValueError(f'Truncated field at {self.offset:#x}')
        value = self.data[self.offset:self.offset+size]
        self.offset += size
        return value

    def integer(self):
        return struct.unpack('>i', self.take(4))[0]

    def count(self):
        value = self.integer()
        if not 0 <= value <= 1000000 or value > (len(self.data)-self.offset)//4:
            raise ValueError(f'Invalid count {value} at {self.offset-4:#x}')
        return value

    def string(self):
        length = self.integer()
        if not length:
            return ''
        wide = length < 0
        raw = self.take(abs(length)*(2 if wide else 1))
        null = b'\0\0' if wide else b'\0'
        if not raw.endswith(null):
            raise ValueError('FString terminator missing')
        return raw[:-len(null)].decode(('utf-16-le' if self.wide_endian=='little' else 'utf-16-be') if wide else 'latin-1')


def inventory(data, allow_aligned_zero_padding=False, wide_endian='little'):
    r = Reader(data,wide_endian)
    files = []
    for _ in range(r.count()):
        name = r.string()
        sections, pairs = r.count(), 0
        for _ in range(sections):
            r.string()  # Section names and values stay private/local.
            count = r.count()
            for _ in range(count):
                r.string()
                r.string()
            pairs += count
        files.append(dict(name=name, sections=sections, pairs=pairs))
    padding=data[r.offset:]
    if padding and not (allow_aligned_zero_padding and len(padding)<16 and len(data)%16==0 and not any(padding)):
        raise ValueError(f'Trailing bytes: {len(data)-r.offset}')
    return dict(size=len(data), sha256=hashlib.sha256(data).hexdigest(), files=files,
                sections=sum(row['sections'] for row in files), pairs=sum(row['pairs'] for row in files),
                consumed=r.offset, trailing_zero_padding=len(padding),
                wide_endian=wide_endian,
                interpretation='BE counts, explicit UTF-16 byte order; no value export; zero padding accepted only when explicitly requested')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('input', type=Path)
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--allow-aligned-zero-padding', action='store_true')
    p.add_argument('--wide-endian', choices=('little','big'), default='little')
    a = p.parse_args()
    if a.out.exists():
        p.error('Choose a fresh report path')
    result = inventory(a.input.read_bytes(), a.allow_aligned_zero_padding, a.wide_endian)
    a.out.write_text(json.dumps(result, indent=2), encoding='utf-8')
    print(f'{len(result["files"])} config files, {result["sections"]} sections, {result["pairs"]} pairs; maps {result["consumed"]} bytes, trailing zero padding {result["trailing_zero_padding"]} bytes')


if __name__ == '__main__':
    main()
