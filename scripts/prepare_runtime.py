"""Apply narrow, checked compatibility fixes to a build-local SDK source copy."""
import argparse
import hashlib
from pathlib import Path
from compat_source import resolve_local_includes, write_if_changed
from prepare_edat import prepare as prepare_edat
from prepare_spu_dispatch import prepare as prepare_spu_dispatch
from prepare_spu_tasks import workload as prepare_tasks, prepare_spurs
from prepare_gcm import prepare as prepare_gcm
from prepare_memory import prepare as prepare_memory
from prepare_renderer import prepare as prepare_renderer
from prepare_input import prepare as prepare_input


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
    prepare_spu_dispatch(a.sdk, a.out.parent)
    prepare_spurs(a.sdk, a.out.parent)
    prepare_gcm(a.sdk, a.out.parent)
    prepare_renderer(a.sdk, a.out.parent)
    prepare_input(a.sdk, a.out.parent)
    loader=a.sdk/'runtime/ppu/ppu_loader.cpp'
    loader_text=loader.read_text(encoding='utf-8')
    anchor='        ppu_context* c = g_resv_ctxs[i];\n        if (!c) continue;\n        fprintf(stderr, "[WATCHDOG]'
    if loader_text.count(anchor)!=1:
        raise ValueError('PPU context sample anchor changed')
    loader_text=loader_text.replace(anchor,anchor.replace('        fprintf(stderr,', '        port_trace_ppu_context(c);\n        fprintf(stderr,'))
    loader_text='extern "C" void port_trace_ppu_context(void*);\n'+loader_text
    anchor='void vm_write32(uint64_t a, uint32_t v) {'
    if loader_text.count(anchor)!=1: raise ValueError('PPU write trace anchor changed')
    loader_text=loader_text.replace(anchor,anchor+' port_gpu_ppu_write(a,v,__builtin_return_address(0));')
    anchor='void vm_write64(uint64_t a, uint64_t v) {'
    if loader_text.count(anchor)!=1: raise ValueError('PPU 64-bit write trace anchor changed')
    loader_text=loader_text.replace(anchor,anchor+' port_gpu_ppu_write(a,(unsigned int)(v>>32),__builtin_return_address(0)); port_gpu_ppu_write(a+4,(unsigned int)v,__builtin_return_address(0));')
    loader_text='extern "C" void port_gpu_ppu_write(unsigned long long,unsigned int,void*);\n'+loader_text
    anchor='    } while (0)\n'
    # Notify only after the shared-memory write and coherence publication.
    begin=loader_text.index('#define VM_WRITE_COH(')
    end=loader_text.index(anchor,begin)
    loader_text=loader_text[:end]+'        port_gcm_notify_write((uint32_t)(a),(uint32_t)(n)); \\\n'+loader_text[end:]
    loader_text='extern "C" void port_gcm_notify_write(unsigned int,unsigned int);\n'+loader_text
    anchor='    if (ok) ppu_resv_break(ea);'
    if loader_text.count(anchor)!=1: raise ValueError('Resource atomic trace anchor changed')
    loader_text=loader_text.replace(anchor,anchor+'\n    if (ok) port_resource_atomic(ea,expected,val,self,__builtin_return_address(0));')
    loader_text='extern "C" void port_resource_atomic(unsigned long long,unsigned int,unsigned int,void*,void*);\n'+loader_text
    anchor='extern "C" void ps3_indirect_call(ppu_context* ctx)\n{'
    if loader_text.count(anchor)!=1: raise ValueError('Indirect allocation trace anchor changed')
    loader_text=loader_text.replace(anchor,anchor+'\n    port_trace_allocation_call(ctx,__builtin_return_address(0));')
    loader_text='extern "C" void port_trace_allocation_call(void*,void*);\n'+loader_text
    loader_text = resolve_local_includes(loader_text, loader)
    loader_out=a.out.parent/'ppu_loader_compat.cpp'
    anchor='extern "C" void lv2_syscall(ppu_context* ctx)\n{'
    if loader_text.count(anchor)!=1: raise ValueError('Syscall profile anchor changed')
    loader_text=loader_text.replace(anchor,'#include "'+(Path(__file__).resolve().parent.parent/'port/diagnostics/hle_profile.h').as_posix()+'"\n'+anchor+'\n    PortHleProfile port_profile(0x80000000u|(unsigned)ctx->gpr[11],ctx);')
    write_if_changed(loader_out, loader_text)
    hle_text=(a.sdk/'runtime/ppu/ppu_hle.cpp').read_text(encoding='utf-8')
    anchor='extern "C" void ps3_hle_call(uint32_t nid, ppu_context* ctx)\n{'
    if hle_text.count(anchor)!=1: raise ValueError('HLE profile anchor changed')
    hle_text=hle_text.replace(anchor,anchor+'\n    PortHleProfile port_profile(nid,ctx);')
    hle_text=hle_text.replace(anchor,'#include "'+(Path(__file__).resolve().parent.parent/'port/diagnostics/hle_profile.h').as_posix()+'"\n'+anchor)
    hle_text = resolve_local_includes(hle_text, a.sdk/'runtime/ppu/ppu_hle.cpp')
    hle_out=a.out.parent/'ppu_hle_compat.cpp'
    write_if_changed(hle_out, hle_text)
    event_source=a.sdk/'runtime/syscalls/sys_event.c'
    event_text=event_source.read_text(encoding='utf-8')
    anchor='    uint64_t timeout_us  = LV2_ARG_U64(ctx, 2);'
    # Only the queue-receive function has this exact declaration.
    if event_text.count(anchor)!=1:
        raise ValueError('Event receive diagnostic anchor changed')
    event_text=event_text.replace(anchor,anchor+'\n    port_event_wait_trace(ctx);')
    event_text='void port_event_wait_trace(void*);\n'+event_text
    event_text = resolve_local_includes(event_text, event_source)
    event_out=a.out.parent/'sys_event_compat.c'
    write_if_changed(event_out, event_text)
    source = a.sdk / 'runtime/ppu/ppu_sysprx.cpp'
    raw = source.read_bytes()
    text = patch_sysprx(raw.decode('utf-8'))
    # The output is outside the original source directory.
    for relative in ('../platform/win32_compat.h', '../memory/vm.h'):
        text = text.replace('"'+relative+'"', '"'+(source.parent/relative).resolve().as_posix()+'"')
    write_if_changed(a.out, text)
    print(f'Prepared lwcond ABI correction from SDK SHA256 {hashlib.sha256(raw).hexdigest()}')
    fs_source = a.sdk / 'runtime/ppu/ppu_fs.cpp'
    fs_text = fs_source.read_text(encoding='utf-8')
    fs_text = fs_text.replace('const char* use = edat_resolve(hpath, dec_path, sizeof dec_path);',
                              'const char* use = port_edat_resolve(hpath, dec_path, sizeof dec_path);')
    old = 'if (use != hpath) snprintf(hpath, sizeof hpath, "%s", use); }'
    if fs_text.count(old) != 2:
        raise ValueError('SDK filesystem resolve sites changed')
    fs_text = fs_text.replace(old, old+'\n    if (edat_is_npd(hpath)) port_resource_blocker(gpath);')
    # A failed decode must never expose ciphertext as a cooked UE3 package.
    fs_text = ('#include <stddef.h>\n'
               'extern "C" const char* port_edat_resolve(const char*, char*, size_t);\n'
               'extern "C" [[noreturn]] void port_resource_blocker(const char*);\n')+fs_text
    marker = '/* ---- handlers ---- */'
    if fs_text.count(marker) != 1:
        raise ValueError('FS host-path bridge anchor changed')
    fs_text = fs_text.replace(marker,
        'extern "C" void port_fs_host_path(char* out, size_t cap, const char* guest) { host_path(out, cap, guest); }\n'+marker)
    for relative in ('../../libs/filesystem/edat.h', '../platform/win32_dirent.h', '../platform/win32_backtrace.h'):
        fs_text = fs_text.replace('"'+relative+'"', '"'+(fs_source.parent/relative).resolve().as_posix()+'"')
    fs_out = a.out.parent/'ppu_fs.cpp'
    write_if_changed(fs_out, fs_text)
    prepare_edat(a.sdk, a.out.parent/'edat_compat.c')
    workload = a.sdk/'runtime/spu/spu_workload.c'
    text = workload.read_text(encoding='utf-8')
    text = prepare_tasks(text)
    anchor = '        return 0;\n    }\n    /* Two lines plus two explicit flushes PER JOB.'
    if text.count(anchor) != 1:
        raise ValueError('SPU job-miss anchor changed')
    text = text.replace(anchor, '        port_spu_job_missing(fp, image_size, job_ea);\n'+anchor)
    text = '#include <stdint.h>\nvoid port_spu_job_missing(uint64_t, uint32_t, uint32_t);\n'+text
    text = resolve_local_includes(text, workload)
    out = a.out.parent/'spu_workload_compat.c'
    write_if_changed(out, text)
    # One tracked record per possible 64-KiB page in the 256-MiB window.
    # Only sys_memory.c consumes this array; keep other SDK files untouched.
    memory_header=a.sdk/'runtime/syscalls/sys_memory.h'
    text=memory_header.read_text(encoding='utf-8')
    old='#define SYS_MEMORY_ALLOC_MAX      1024'
    if text.count(old)!=1:
        raise ValueError('Memory allocation table declaration changed')
    text=text.replace(old,'#define SYS_MEMORY_ALLOC_MAX      4096')
    text = resolve_local_includes(text, memory_header)
    header_out=a.out.parent/'sys_memory_compat.h'
    write_if_changed(header_out, text)
    source=a.sdk/'runtime/syscalls/sys_memory.c'
    text=source.read_text(encoding='utf-8').replace('"sys_memory.h"','"'+header_out.resolve().as_posix()+'"')
    text=prepare_memory(text)
    out=a.out.parent/'sys_memory_compat.c'
    write_if_changed(out, text)


if __name__ == '__main__':
    main()
