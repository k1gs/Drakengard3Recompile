"""Reuse only explicitly freed sys_memory pages in the existing guest window."""
from prepare_spu_tasks import once


def prepare(text):
    text = once(text, 'static uint32_t s_total_allocated = 0;', '''void ppu_dump_guest_stack(ppu_context*,const char*);
static uint32_t s_total_allocated = 0;
/* A set byte denotes an explicitly freed 64-KiB page, not virgin VA.
 * This avoids overlap with mmapper reservations sharing the bump pointer. */
static uint8_t port_freed_pages[(SYS_MEM_ALLOC_END-SYS_MEM_ALLOC_BASE)/0x10000u];
static unsigned port_memory_frees;
static uint32_t port_reuse_pages(uint32_t size, uint32_t alignment) {
    unsigned pages=size/0x10000u, stride=alignment/0x10000u;
    for (unsigned i=0; i+pages<=sizeof(port_freed_pages); i+=stride) {
        unsigned j=0;
        while (j<pages && port_freed_pages[i+j]) ++j;
        if (j==pages) {
            memset(port_freed_pages+i,0,pages);
            return SYS_MEM_ALLOC_BASE+i*0x10000u;
        }
    }
    return 0;
}
static void port_release_pages(uint32_t addr,uint32_t size) {
    memset(port_freed_pages+(addr-SYS_MEM_ALLOC_BASE)/0x10000u,1,size/0x10000u);
}''')
    start = text.index('    /* Align bump pointer */', text.index('int64_t sys_memory_allocate('))
    end = text.index('    /* Find a free allocation slot */', start)
    text = text[:start] + text[end:]
    text = once(text, '''    uint32_t alloc_addr = g_sys_mem_bump_ptr;
    g_sys_mem_bump_ptr += size;
    s_total_allocated += size;''', '''    uint32_t alloc_addr = port_reuse_pages(size,alignment);
    if (!alloc_addr) {
        uint64_t aligned=((uint64_t)g_sys_mem_bump_ptr+alignment-1)&~((uint64_t)alignment-1);
        if (aligned+size>SYS_MEM_ALLOC_END) {
            fprintf(stderr,"[port-memory] window exhausted bump=%08X request=%08X live=%08X frees=%u\\n",
                g_sys_mem_bump_ptr,size,s_total_allocated,port_memory_frees);
            fprintf(stderr,"[port-memory] caller tid=%llu lr=%08llX raw-size=%08X\\n",
                (unsigned long long)ctx->thread_id,(unsigned long long)ctx->lr,LV2_ARG_U32(ctx,0));
            ppu_dump_guest_stack(ctx,"memory-exhaustion");
            bump_unlock();
            return (int64_t)(int32_t)CELL_ENOMEM;
        }
        alloc_addr=(uint32_t)aligned;
        g_sys_mem_bump_ptr=alloc_addr+size;
    }
    s_total_allocated += size;''')
    text = once(text, '''    bump_unlock();

    /* Commit the pages (window is outside the pre-committed main region);
     * fresh commits are already zeroed by the OS */
    if (vm_commit(alloc_addr, size) != CELL_OK) {
        a->active = 0;
        return (int64_t)(int32_t)CELL_ENOMEM;
    }''', '''    /* Keep slot ownership and accounting locked through commit/rollback. */
    if (vm_commit(alloc_addr, size) != CELL_OK) {
        a->active = 0;
        s_total_allocated -= size;
        port_release_pages(alloc_addr,size);
        bump_unlock();
        return (int64_t)(int32_t)CELL_ENOMEM;
    }
    memset(vm_to_host(alloc_addr),0,size);
    bump_unlock();''')
    begin = text.index('int64_t sys_memory_free(')
    end = text.index('/* ---------------------------------------------------------------------------',begin)
    original = text[begin:end]
    patched = once(original, '    for (int i = 0;', '    bump_lock();\n    for (int i = 0;')
    patched = once(patched, '            s_total_allocated -=',
        '            ++port_memory_frees;\n            port_release_pages(addr,g_sys_mem_allocs[i].size);\n            s_total_allocated -=')
    patched = once(patched, '            return CELL_OK;', '            bump_unlock();\n            return CELL_OK;')
    patched = once(patched, '    return (int64_t)(int32_t)CELL_EINVAL;',
        '    bump_unlock();\n    return (int64_t)(int32_t)CELL_EINVAL;')
    return text[:begin]+patched+text[end:]
