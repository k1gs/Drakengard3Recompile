#include "ppu_recomp.h"
#include "ps3emu/nid.h"
#include <cstdio>
#include <cstring>
#include <array>
#include <thread>
#include <chrono>

extern "C" unsigned char* vm_base;
extern "C" void ppu_sysprx_register();
extern "C" void ps3_hle_call(uint32_t, ppu_context*);
extern "C" uint32_t vm_read32(uint64_t);
extern "C" int64_t sys_memory_allocate(ppu_context*);
extern "C" int64_t sys_memory_free(ppu_context*);
extern "C" void port_register_queue_api();
extern "C" void vm_write32(uint64_t,uint32_t);
extern "C" void spu_lockline_lock();
extern "C" void spu_lockline_unlock();

extern "C" int port_check_queue()
{
    constexpr uint32_t q=0x50000,out=0x50100;
    memset(vm_base+q,0,128); memset(vm_base+out,0xA5,16);
    vm_write32(q+0x0C,2); vm_write32(q+0x10,16); vm_write32(q+0x14,2); vm_write32(q+0x1C,0x50200);
    port_register_queue_api();
    ppu_context ctx{}; ctx.gpr[1]=0x60000;
    auto call=[&](uint32_t queue,uint32_t buffer) {
        ctx.gpr[3]=queue; ctx.gpr[4]=buffer; ctx.gpr[5]=0; ctx.gpr[6]=0;
        ps3_hle_call(ps3_compute_nid("cellSpursQueuePopBody"),&ctx);
        return (uint32_t)ctx.gpr[3];
    };
    bool ok=call(q,out)==0x80410901 && call(0,out)==0x80410911 && call(q+1,out)==0x80410910;
    for(unsigned i=0;i<16;i++) if(vm_base[out+i]!=0xA5) ok=false;
    for(unsigned i=0;i<32;i++) vm_base[0x50200+i]=(uint8_t)i;
    vm_write32(q+4,2);
    ok=ok && call(q,out)==0 && vm_read32(q)==1 && !memcmp(vm_base+out,vm_base+0x50200,16);
    ok=ok && call(q,out)==0 && vm_read32(q)==2 && !memcmp(vm_base+out,vm_base+0x50210,16);
    vm_write32(q,3); vm_write32(q+4,0);
    ok=ok && call(q,out)==0 && vm_read32(q)==0 && !memcmp(vm_base+out,vm_base+0x50210,16);
    // An actual producer publishes after the blocking consumer starts waiting.
    std::thread producer([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        spu_lockline_lock();
        vm_base[q+4]=0; vm_base[q+5]=0; vm_base[q+6]=0; vm_base[q+7]=1;
        spu_lockline_unlock();
    });
    ctx.gpr[3]=q; ctx.gpr[4]=out; ctx.gpr[5]=0; ctx.gpr[6]=1;
    ps3_hle_call(ps3_compute_nid("cellSpursQueuePopBody"),&ctx);
    producer.join();
    ok=ok && !ctx.gpr[3] && vm_read32(q)==1 && !memcmp(vm_base+out,vm_base+0x50200,16);
    fprintf(stderr,"[compat-check] queue errors, untouched empty output, FIFO, wrap, blocking producer: %s\n",ok?"PASS":"FAIL");
    // The LF queue has a separate ABI. Validate the full fresh ANY2ANY line,
    // neighboring bytes and preservation of the caller's backing buffer.
    constexpr uint32_t lf=0x51000,buf=0x51100;
    memset(vm_base+lf-16,0xA5,160); memset(vm_base+lf,0,128); memset(vm_base+buf,0xA5,512);
    ctx.gpr[3]=0x58001;ctx.gpr[4]=lf;ctx.gpr[5]=buf;ctx.gpr[6]=32;ctx.gpr[7]=16;ctx.gpr[8]=3;
    ps3_hle_call(ps3_compute_nid("_cellSpursLFQueueInitialize"),&ctx);
    std::array<uint32_t,32> expected{};
    expected[4]=32;expected[5]=16;expected[7]=buf|1;
    expected[8]=0xFFFF0000u;expected[9]=3;expected[10]=0xFFFFFFFFu;
    expected[12]=expected[20]=0xFFFF0000u;expected[29]=0x58001;
    ok=ok && !ctx.gpr[3];
    for(unsigned i=0;i<32;i++) if(vm_read32(lf+4*i)!=expected[i]) ok=false;
    for(unsigned i=0;i<16;i++) if(vm_base[lf-16+i]!=0xA5 || vm_base[lf+128+i]!=0xA5) ok=false;
    for(unsigned i=0;i<512;i++) if(vm_base[buf+i]!=0xA5) ok=false;
    fprintf(stderr,"[compat-check] fresh LF ANY2ANY full 128-byte ABI, tagged buffer, canaries: %s\n",ok?"PASS":"FAIL");
    return ok?0:1;
}

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
    // More allocation/free cycles than there are pages in the fixed window.
    // This must fit because only one page remains live, irrespective of history.
    uint32_t first=0;
    for(unsigned i=0;i<4097;i++) {
        ctx.gpr[3]=0x10000; ctx.gpr[4]=0x200; ctx.gpr[5]=0x32000;
        if(sys_memory_allocate(&ctx)) {
            fprintf(stderr,"[compat-check] freed-page reuse FAIL at cycle %u\n",i);
            return 4;
        }
        uint32_t address=vm_read32(0x32000);
        if(!i) first=address;
        for(unsigned k=0;k<0x10000;k++) if(vm_base[address+k]) return 5;
        memset(vm_base+address,0xA5,0x10000);
        ctx.gpr[3]=address;
        if(sys_memory_free(&ctx) || (uint32_t)sys_memory_free(&ctx)!=0x80010002u) return 6;
    }
    ctx.gpr[3]=0x100000; ctx.gpr[4]=0x400; ctx.gpr[5]=0x32000;
    if(sys_memory_allocate(&ctx) || (vm_read32(0x32000)&0xFFFFF)) return 7;
    ctx.gpr[3]=vm_read32(0x32000);
    if(sys_memory_free(&ctx)) return 8;
    fprintf(stderr,"[compat-check] 4097 allocate/free cycles, zeroed reuse, double-free rejection, 1-MiB alignment: PASS (first=%08X)\n",first);
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
