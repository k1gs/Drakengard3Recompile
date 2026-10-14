"""Replace silent unsupported PPU operations with an explicit runtime stop.

Requires the original audit hashes and preserves changed chunks before editing.
Game-derived sources and backups belong only under ignored work/.
"""
import argparse
import hashlib
import json
from pathlib import Path


def guard(directory, audit):
    by_file = {}
    for finding in audit['findings']:
        by_file.setdefault(finding['file'], []).append(finding)
    pending = []
    hashes = {row['name']: row['sha256'] for row in audit['chunks']}
    for name, findings in by_file.items():
        path = directory/name
        raw = path.read_bytes()
        if hashlib.sha256(raw).hexdigest() != hashes[name]:
            raise ValueError(f'Chunk changed since audit: {name}')
        lines = raw.decode('utf-8', errors='surrogateescape').splitlines(keepends=True)
        for row in findings:
            index = row['line']-1
            line = lines[index]
            function = int(row['function'].split('_')[1], 16)
            detail = json.dumps(row['detail'])
            call = f'port_unimplemented_instruction(0x{function:08X}ULL, {detail});'
            if row['kind'] == 'todo':
                if line.strip() != f"/* TODO: {row['detail']} */;":
                    raise ValueError(f'Unexpected TODO shape: {name}:{index+1}')
                lines[index] = '        '+call+'\n'
            elif '-> non-code' in line and line.count('return;') == 1:
                # Preserve the original condition; fail only on the taken edge.
                lines[index] = line.replace('return;', call)
            else:
                raise ValueError(f'Unsupported diagnostic shape: {name}:{index+1}')
        declaration = 'extern "C" [[noreturn]] void port_unimplemented_instruction(unsigned long long, const char*);\n'
        pending.append((path, raw, declaration+''.join(lines)))
    backup = directory/'original-unguarded'
    if backup.exists():
        raise ValueError('Backup exists; use a fresh lift/audit rather than overwriting')
    backup.mkdir()
    for path, raw, text in pending:
        (backup/path.name).write_bytes(raw)
        path.write_bytes(text.encode('utf-8', errors='surrogateescape'))
    return len(pending), sum(map(len, by_file.values()))


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('directory', type=Path)
    p.add_argument('--audit', type=Path, required=True)
    a = p.parse_args()
    chunks, sites = guard(a.directory, json.loads(a.audit.read_text(encoding='utf-8')))
    print(f'Guarded {sites} sites in {chunks} chunks; original files backed up')


if __name__ == '__main__':
    main()
