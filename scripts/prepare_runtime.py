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
from prepare_depth_surfaces import prepare_commands as prepare_depth_commands


def patch_sysprx(text):
    start = text.index('/* sys_lwcond (sysPrxForUser)')
    end = text.index('/* sys_ppu_thread_get_id(', start)
    old = text[start:end]
    if 'ReleaseSemaphore(s, 1, NULL);\n        Sleep(1);' not in old or old.count('static void sys_lwcond_') != 6:
        raise ValueError('Pinned sysPrxForUser condition implementation changed')
    source = Path(__file__).resolve().parents[1] / 'port' / 'lwcond_wait.inc'
    return text[:start] + source.read_text(encoding='utf-8') + '\n\n' + text[end:]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--sdk', type=Path, required=True)
    p.add_argument('--out', type=Path, required=True)
    a = p.parse_args()
    prepare_spu_dispatch(a.sdk, a.out.parent)
    prepare_spurs(a.sdk, a.out.parent)
    prepare_gcm(a.sdk, a.out.parent)
    prepare_renderer(a.sdk, a.out.parent)
    prepare_depth_commands(a.sdk, a.out.parent)
    prepare_input(a.sdk, a.out.parent)
    rsx = a.sdk / 'libs/video/sys_rsx.c'
    driver = rsx.read_text(encoding='utf-8')
    iomap = '    cellGcm_syscall_iomap(ea, io, size);'
    if driver.count(iomap) != 1:
        raise ValueError('Pinned RSX context IO mapping changed')
    driver = driver.replace(iomap, '''    if (port_invalid_user_mapping(ea, size))
        return (int64_t)(int32_t)CELL_EINVAL;
''' + iomap)
    driver = '#include "runtime/service_memory.h"\n' + resolve_local_includes(driver, rsx, a.sdk)
    driver += '''
void port_rsx_display_queued(u32 head, u32 buffer)
{
    if (!s_isr_qid) return;
    const u32 base = RSX_DRIVER_INFO_EA + RSX_DI_HEAD_BASE + head * RSX_DI_HEAD_STRIDE;
    vm_write32(base + RSX_DI_HEAD_LASTQUEUED, buffer);
    const u32 flags = vm_read32(base + RSX_DI_HEAD_FLIPFLAGS);
    vm_write32(base + RSX_DI_HEAD_FLIPFLAGS, flags | 0x40000000u | (1u << buffer));
    rsx_send_event(SYS_RSX_EVENT_QUEUE_BASE << head);
}
'''
    write_if_changed(a.out.parent / 'sys_rsx_compat.c', driver)
    loader=a.sdk/'runtime/ppu/ppu_loader.cpp'
    loader_text=loader.read_text(encoding='utf-8')
    # These fallback mmapper calls return guest-owned memory. Reject a span
    # crossing into the driver's window before changing the allocator cursor.
    for cursor, begin, end, candidate in (
            ('s_next_va', '        s_next_va = (s_next_va + align - 1)',
             '        s_next_va += (size + 0xFFFFFu) & ~0xFFFFFu;',
             '(s_next_va + align - 1) & ~((uint64_t)align - 1)'),
            ('s_map_next', '        uint32_t va = s_map_next;',
             '        s_map_next = (s_map_next + size + 0xFFFFFu) & ~0xFFFFFu;',
             's_map_next ? s_map_next : (start ? start : 0x11000000u)')):
        if loader_text.count(begin) != 1 or loader_text.count(end) != 1:
            raise ValueError('Pinned guest mapper cursor changed: ' + cursor)
        first = loader_text.index(begin)
        last = loader_text.index(end, first) + len(end)
        loader_text = loader_text[:first] + f'''        const uint64_t candidate = {candidate};
        const uint64_t next = (candidate + size + 0xFFFFFu) & ~UINT64_C(0xFFFFF);
        if (port_invalid_user_mapping(candidate, next - candidate)) {{
            ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)CELL_ENOMEM;
            return;
        }}
        const uint32_t va = (uint32_t)candidate;
        {cursor} = next;''' + loader_text[last:]
        loader_text = loader_text.replace('static uint32_t ' + cursor + ' =',
                                          'static uint64_t ' + cursor + ' =', 1)
    loader_text = loader_text.replace('        if (!s_map_next) s_map_next = start ? start : 0x11000000u;\n', '')
    reserve = '        static uint64_t s_next_va = 0x11000000u;'
    if loader_text.count(reserve) != 1:
        raise ValueError('Pinned guest address reservation changed')
    loader_text = loader_text.replace(reserve, '''        if (ctx->gpr[3] > UINT32_MAX || ctx->gpr[5] > UINT32_MAX) {
            ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)CELL_ENOMEM;
            return;
        }
''' + reserve)
    loader_text = '#include "runtime/service_memory.h"\n#include "include/ps3emu/error_codes.h"\n' + loader_text
    # The PPU may poll REF entirely in lifted code. Deliver already-pending
    # display interrupts on the polling guest thread, also when ref pacing is off.
    poll = 'if ((uint32_t)a == (VM_HLE_INJECT_BASE + 0x2008u)) { static int _rp=-1;'
    if loader_text.count(poll) != 1:
        raise ValueError('Pinned RSX reference poll changed')
    loader_text = loader_text.replace(poll, '''if ((uint32_t)a == (VM_HLE_INJECT_BASE + 0x2008u)) {
        if (g_active_ctx) ppu_gcm_pump();
        static int _rp=-1;''')
    loader_text = 'extern "C" void ppu_gcm_pump(void);\n' + loader_text
    anchor='        ppu_context* c = g_resv_ctxs[i];\n        if (!c) continue;\n        fprintf(stderr, "[WATCHDOG]'
    if loader_text.count(anchor)!=1:
        raise ValueError('PPU context sample anchor changed')
    loader_text=loader_text.replace(anchor,anchor.replace('        fprintf(stderr,', '        port_trace_ppu_context(c);\n        fprintf(stderr,'))
    loader_text='extern "C" void port_trace_ppu_context(void*);\n'+loader_text
    anchor='void vm_write32(uint64_t a, uint32_t v) {'
    if loader_text.count(anchor)!=1: raise ValueError('PPU write trace anchor changed')
    loader_text=loader_text.replace(anchor,anchor+' if(port_gpu_ppu_write32_needed(a)) port_gpu_ppu_write(a,v,__builtin_return_address(0));')
    anchor='void vm_write64(uint64_t a, uint64_t v) {'
    if loader_text.count(anchor)!=1: raise ValueError('PPU 64-bit write trace anchor changed')
    loader_text=loader_text.replace(anchor,anchor+' if(port_gpu_ppu_write64_needed(a)) { port_gpu_ppu_write(a,(unsigned int)(v>>32),__builtin_return_address(0)); port_gpu_ppu_write(a+4,(unsigned int)v,__builtin_return_address(0)); }')
    loader_text='extern "C" void port_gpu_ppu_write(unsigned long long,unsigned int,void*);\n'+loader_text
    loader_text='#include "diagnostics/gpu_writer_gate.h"\n'+loader_text
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
    loader_text = resolve_local_includes(loader_text, loader, a.sdk)
    loader_out=a.out.parent/'ppu_loader_compat.cpp'
    anchor='extern "C" void lv2_syscall(ppu_context* ctx)\n{'
    if loader_text.count(anchor)!=1: raise ValueError('Syscall profile anchor changed')
    loader_text=loader_text.replace(anchor,'#include "diagnostics/hle_profile.h"\n'+anchor+'\n    PortHleProfile port_profile(0x80000000u|(unsigned)ctx->gpr[11],ctx);')
    write_if_changed(loader_out, loader_text)
    hle_text=(a.sdk/'runtime/ppu/ppu_hle.cpp').read_text(encoding='utf-8')
    anchor='extern "C" void ps3_hle_call(uint32_t nid, ppu_context* ctx)\n{'
    if hle_text.count(anchor)!=1: raise ValueError('HLE profile anchor changed')
    hle_text=hle_text.replace(anchor,anchor+'\n    PortHleProfile port_profile(nid,ctx);')
    hle_text=hle_text.replace(anchor,'#include "diagnostics/hle_profile.h"\n'+anchor)
    hle_text = resolve_local_includes(hle_text, a.sdk/'runtime/ppu/ppu_hle.cpp', a.sdk)
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
    event_text = resolve_local_includes(event_text, event_source, a.sdk)
    event_out=a.out.parent/'sys_event_compat.c'
    write_if_changed(event_out, event_text)
    source = a.sdk / 'runtime/ppu/ppu_sysprx.cpp'
    raw = source.read_bytes()
    text = patch_sysprx(raw.decode('utf-8'))
    callback='''extern "C" void cellGcm_fifo_recycle(unsigned int ctx_ea);'''
    call='''    cellGcm_fifo_recycle((unsigned int)ctx->gpr[3]);   /* r3 = context EA */
    ctx->gpr[3] = 0;                                   /* CELL_OK */'''
    if text.count(callback)!=1 or text.count(call)!=1:
        raise ValueError('Pinned FIFO callback ABI changed')
    text=text.replace(callback,'''extern "C" int32_t cellGcm_fifo_recycle(unsigned int ctx_ea);''')
    text=text.replace(call,'''    ctx->gpr[3] = (uint32_t)cellGcm_fifo_recycle((unsigned int)ctx->gpr[3]);''')
    # The output is outside the original source directory.
    text = resolve_local_includes(text, source, a.sdk)
    write_if_changed(a.out, text)
    print(f'Prepared lwcond wait and signal from SDK SHA256 {hashlib.sha256(raw).hexdigest()}')
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
    fs_text = resolve_local_includes(fs_text, fs_source, a.sdk)
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
    text = resolve_local_includes(text, workload, a.sdk)
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
    text = resolve_local_includes(text, memory_header, a.sdk)
    header_out=a.out.parent/'sys_memory_compat.h'
    write_if_changed(header_out, text)
    source=a.sdk/'runtime/syscalls/sys_memory.c'
    text=source.read_text(encoding='utf-8').replace('"sys_memory.h"','"sys_memory_compat.h"')
    text=prepare_memory(text)
    out=a.out.parent/'sys_memory_compat.c'
    write_if_changed(out, text)


if __name__ == '__main__':
    main()
