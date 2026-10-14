"""Apply narrow, checked compatibility fixes to a build-local SDK source copy."""
import argparse
import hashlib
from pathlib import Path


def patch_sysprx(text):
    changes = [
        ('sys_lwcond_t: +0x00 lwmutex EA (be64),\n * +0x08 lwcond_queue id.',
         'sys_lwcond_t: +0x00 lwmutex EA (be32),\n * +0x04 lwcond_queue id. The guest ABI is eight bytes.'),
        ('vm_write64(lwcond + 0x00, (uint64_t)lwmutex);',
         'vm_write32(lwcond + 0x00, lwmutex);'),
        ('vm_write32(lwcond + 0x08, ++s_lwcond_id);',
         'vm_write32(lwcond + 0x04, ++s_lwcond_id);'),
        ('uint32_t lwmutex = (uint32_t)vm_read64(lwcond + 0x00);',
         'uint32_t lwmutex = vm_read32(lwcond + 0x00);'),
    ]
    for old, new in changes:
        if text.count(old) != 1:
            raise ValueError(f'SDK source changed: expected one occurrence of {old!r}')
        text = text.replace(old, new)
    return text


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--sdk', type=Path, required=True)
    p.add_argument('--out', type=Path, required=True)
    a = p.parse_args()
    source = a.sdk / 'runtime/ppu/ppu_sysprx.cpp'
    raw = source.read_bytes()
    text = patch_sysprx(raw.decode('utf-8'))
    # The output is outside the original source directory.
    for relative in ('../platform/win32_compat.h', '../memory/vm.h'):
        text = text.replace('"'+relative+'"', '"'+(source.parent/relative).resolve().as_posix()+'"')
    if not a.out.exists() or a.out.read_text(encoding='utf-8') != text:
        a.out.write_text(text, encoding='utf-8')
    print(f'Prepared lwcond ABI correction from SDK SHA256 {hashlib.sha256(raw).hexdigest()}')
    fs_source = a.sdk / 'runtime/ppu/ppu_fs.cpp'
    fs_text = fs_source.read_text(encoding='utf-8')
    old = 'if (use != hpath) snprintf(hpath, sizeof hpath, "%s", use); }'
    if fs_text.count(old) != 2:
        raise ValueError('SDK filesystem resolve sites changed')
    fs_text = fs_text.replace(old, old+'\n    if (edat_is_npd(hpath)) port_resource_blocker(gpath);')
    # A failed decode must never expose ciphertext as a cooked UE3 package.
    fs_text = 'extern "C" [[noreturn]] void port_resource_blocker(const char*);\n'+fs_text
    for relative in ('../../libs/filesystem/edat.h', '../platform/win32_dirent.h', '../platform/win32_backtrace.h'):
        fs_text = fs_text.replace('"'+relative+'"', '"'+(fs_source.parent/relative).resolve().as_posix()+'"')
    fs_out = a.out.parent/'ppu_fs.cpp'
    if not fs_out.exists() or fs_out.read_text(encoding='utf-8') != fs_text:
        fs_out.write_text(fs_text, encoding='utf-8')


if __name__ == '__main__':
    main()
