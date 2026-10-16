#include "ppu_recomp.h"
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
static std::mutex trace_mutex;
static std::map<uint32_t,unsigned> parks;
static unsigned records=0;
static bool enabled() { static const bool value=std::getenv("PORT_GPU_WRITERS")!=nullptr; return value; }
extern "C" void port_gpu_ppu_write(unsigned long long address,unsigned int value,void* caller)
{
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
