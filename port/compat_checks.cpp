#include "ppu_recomp.h"
#include "ps3emu/nid.h"
#include <cstdio>
#include <cstring>
#include <array>

extern "C" unsigned char* vm_base;
extern "C" void ppu_sysprx_register();
extern "C" void ps3_hle_call(uint32_t, ppu_context*);
extern "C" uint32_t vm_read32(uint64_t);
extern "C" int64_t sys_memory_allocate(ppu_context*);
extern "C" int64_t sys_memory_free(ppu_context*);

extern "C" int port_check_memory_table()
{
    std::array<uint32_t,1025> addresses{};
    ppu_context ctx{};
    unsigned count=0;
    for(;count<addresses.size();++count) {
        ctx.gpr[3]=0x10000; ctx.gpr[4]=0x200; ctx.gpr[5]=0x32000;
        int64_t rc=sys_memory_allocate(&ctx);
        if(rc) {
            fprintf(stderr,"[compat-check] memory allocation %u failed rc=%08llX\n",count+1,(unsigned long long)rc);
            return 1;
        }
        addresses[count]=vm_read32(0x32000);
        if(count && addresses[count]<addresses[count-1]+0x10000) return 2;
    }
    for(auto address:addresses) {
        ctx.gpr[3]=address;
        if(sys_memory_free(&ctx)) return 3;
    }
    fprintf(stderr,"[compat-check] 1025 live 64-KiB allocations, unique ranges, free: PASS\n");
    return 0;
}

// Exercise the actual registered handler with guest BE memory and guard bytes.
// This catches the former 64-bit pointer/+8 queue write and its adjacent overwrite.
extern "C" int port_check_lwcond_layout()
{
    constexpr uint32_t condition = 0x00030100, mutex = 0x00030000;
    memset(vm_base+condition-8, 0xA5, 24);
    ppu_context ctx{};
    ctx.thread_id = 1;
    ctx.gpr[1] = 0x40000;
    ctx.gpr[3] = condition;
    ctx.gpr[4] = mutex;
    ppu_sysprx_register();
    ps3_hle_call(ps3_compute_nid("sys_lwcond_create"), &ctx);
    bool ok = ctx.gpr[3] == 0 && vm_read32(condition) == mutex && vm_read32(condition+4) != 0;
    for (int i = -8; i < 16; ++i)
        if ((i < 0 || i >= 8) && vm_base[condition+i] != 0xA5) ok = false;
    fprintf(stderr, "[compat-check] lwcond BE32 pointer, queue ID, adjacent canaries: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
