#include "ppu_recomp.h"
#include "gpu_writer_gate.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <map>
#include <atomic>
extern "C" PPU_THREAD_LOCAL ppu_context* g_active_ctx;
extern "C" uint32_t vm_read32(uint64_t);
extern "C" uint32_t port_gcm_io_ea(uint32_t);
extern "C" uint32_t ppu_prof_resolve_host(void*);
extern "C" void ppu_dump_guest_stack(ppu_context*,const char*);
extern "C" void port_task_arm_write_watch(uint64_t,uint32_t);
static std::mutex trace_mutex;
static std::map<uint32_t,unsigned> parks;
static unsigned records=0;
static bool enabled() { static const bool value=std::getenv("PORT_GPU_WRITERS")!=nullptr; return value; }
extern "C" {
int port_gpu_ppu_write_active = std::getenv("PORT_GPU_WRITERS") != nullptr ||
    std::getenv("PORT_TASK_RESULT_TRACE") != nullptr ||
    std::getenv("PORT_JC_TRACE") != nullptr ||
    std::getenv("PORT_RESOURCE_TRACE") != nullptr;
}
// Only enabled in the isolated native fixture, before any guest threads exist.
static bool check_writer_calls;
static unsigned writer_calls;
extern "C" void port_gpu_ppu_write(unsigned long long address,unsigned int value,void* caller)
{
    if(check_writer_calls) ++writer_calls;
    if(address==port_task_pointer_address) port_task_arm_write_watch(address,value);
    static const bool task=std::getenv("PORT_TASK_RESULT_TRACE")!=nullptr;
    if(task && value==0x800 && address>=0x40000000 && address<0xC0000000) {
        uint32_t base=vm_read32(0x01AC39B8);
        if(base>=0x40000000 && address>=base && address<base+128) {
            auto* ctx=g_active_ctx;
            fprintf(stderr,"[task-pointer-write] ea=%08llX base=%08X old=%08X new=%08X fn=%08X lr=%08llX\n",
                address,base,vm_read32(address),value,ppu_prof_resolve_host(caller),ctx?(unsigned long long)ctx->lr:0);
            if(ctx) ppu_dump_guest_stack(ctx,"task-pointer-write");
        }
    }
    static const bool chains=std::getenv("PORT_JC_TRACE")!=nullptr;
    if(chains && address>=0x01A2A900 && address<0x01A2B900 && (address&7)==4) {
        uint32_t old=vm_read32(address);
        if(old==0x7F || value==0x7F) {
            static std::atomic<unsigned> count{0};
            if(count.fetch_add(1)<1024) {
                auto* ctx=g_active_ctx;
                fprintf(stderr,"[jc-publish] ea=%08llX old=%08X new=%08X fn=%08X lr=%08llX tid=%llu\n",
                    address-4,old,value,ppu_prof_resolve_host(caller),ctx?(unsigned long long)ctx->lr:0,ctx?(unsigned long long)ctx->thread_id:0);
            }
        }
    }
    static const bool resources=std::getenv("PORT_RESOURCE_TRACE")!=nullptr;
    if(resources && (address==0x0197D114 || (address>=0x01AC3218 && address<0x01AC3318))) {
        static std::mutex resource_mutex;
        std::lock_guard<std::mutex> lock(resource_mutex);
        static unsigned count=0;
        if(count++<256) {
            auto* ctx=g_active_ctx;
            fprintf(stderr,"[resource-write] ea=%08llX old=%08X new=%08X fn=%08X lr=%08llX tid=%llu r3=%08llX r4=%08llX r5=%08llX\n",
                address,vm_read32(address),value,ppu_prof_resolve_host(caller),ctx?(unsigned long long)ctx->lr:0,
                ctx?(unsigned long long)ctx->thread_id:0,ctx?(unsigned long long)ctx->gpr[3]:0,
                ctx?(unsigned long long)ctx->gpr[4]:0,ctx?(unsigned long long)ctx->gpr[5]:0);
        }
    }
    if(!enabled() || address<0x40000000 || address>=0x40200000) return;
    uint32_t a=(uint32_t)address,old=vm_read32(a);
    bool create=(value>>29)==1 && port_gcm_io_ea(value&0x1FFFFFFCu)==a;
    bool patch=(old>>29)==1 && port_gcm_io_ea(old&0x1FFFFFFCu)==a && old!=value;
    if(!create && !patch) return;
    std::lock_guard<std::mutex> lock(trace_mutex);
    if(create) parks[a]++;
    if(records++>=4096) return;
    auto* ctx=g_active_ctx;
    fprintf(stderr,"[gpu-writer] PPU %s ea=%08X old=%08X new=%08X fn=%08X lr=%08llX tid=%llu\n",
        create?"park":"patch",a,old,value,ppu_prof_resolve_host(caller),ctx?(unsigned long long)ctx->lr:0,
        ctx?(unsigned long long)ctx->thread_id:0);
    if(create && parks.size()<=2 && ctx) ppu_dump_guest_stack(ctx,"gpu-park-producer");
}
extern "C" void port_gpu_spu_write(unsigned int ea,unsigned int size)
{
    if(!enabled() || !size || (uint64_t)ea+size<0x40000000 || ea>=0x40200000) return;
    std::lock_guard<std::mutex> lock(trace_mutex);
    auto it=parks.lower_bound(ea);
    while(it!=parks.end() && it->first<(uint64_t)ea+size) {
        if(records++<4096) fprintf(stderr,"[gpu-writer] SPU DMA ea=%08X size=%u park=%08X word=%08X\n",ea,size,it->first,vm_read32(it->first));
        ++it;
    }
}

extern "C" int port_check_gpu_writer_gate()
{
    extern void vm_write32(uint64_t,uint32_t);
    extern void vm_write64(uint64_t,uint64_t);
    constexpr uint64_t ea=0x60000000;
    int saved=port_gpu_ppu_write_active;
    check_writer_calls=true;writer_calls=0;port_gpu_ppu_write_active=0;
    for(unsigned i=0;i<1000;i++) {
        vm_write32(ea,i);vm_write64(ea+8,0x1234567800000000ULL+i);
    }
    bool skipped=writer_calls==0;
    bool memory=vm_read32(ea)==999 && vm_read32(ea+8)==0x12345678 && vm_read32(ea+12)==999;
    writer_calls=0;port_gpu_ppu_write_active=1;
    vm_write32(ea,1);vm_write64(ea+8,0x2345678900000002ULL);
    bool enabled_calls=writer_calls==3 && vm_read32(ea+8)==0x23456789 && vm_read32(ea+12)==2;
    writer_calls=0;port_gpu_ppu_write_active=0;
    vm_write32(port_task_pointer_address,0);
    vm_write64(port_task_pointer_address-4,0xAABBCCDD00000001ULL);
    vm_write64(port_task_pointer_address,0x0000000200000003ULL);
    vm_write64(port_task_pointer_address+4,0x0000000400000005ULL);
    bool task=writer_calls==5 && vm_read32(port_task_pointer_address-4)==0xAABBCCDD &&
        vm_read32(port_task_pointer_address)==2 && vm_read32(port_task_pointer_address+4)==4 &&
        vm_read32(port_task_pointer_address+8)==5;
    check_writer_calls=false;
    auto bench=[&](int active) {
        port_gpu_ppu_write_active=active;
        auto start=std::chrono::steady_clock::now();
        for(unsigned i=0;i<1000000;i++) vm_write32(ea,i);
        return std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
    };
    double dispatch_seconds=bench(1),disabled_seconds=bench(0);
    port_gpu_ppu_write_active=saved;
    bool ok=skipped&&memory&&enabled_calls&&task;
    fprintf(stderr,"[compat-check] GPU writer gate launch-active=%d disabled-skips=%d memory=%d enabled-dispatch=%d task32/64-boundaries=%d %s; 1M stores dispatch=%.6fs disabled=%.6fs\n",
        saved,skipped,memory,enabled_calls,task,ok?"PASS":"FAIL",dispatch_seconds,disabled_seconds);
    return ok?0:1;
}
