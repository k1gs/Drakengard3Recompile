#include "ppu_recomp.h"
#include "ps3emu/nid.h"
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <cstring>
#include "win32_compat.h"
extern "C" uint32_t vm_read32(uint64_t);
extern "C" void vm_write32(uint64_t,uint32_t);
extern "C" void ps3_hle_register_ctx(uint32_t,const char*,void (*)(ppu_context*));
extern "C" void spu_lockline_lock();
extern "C" void spu_lockline_unlock();
extern "C" void spu_coh_notify_write(uint32_t);
extern "C" uint8_t* vm_base;
extern "C" [[noreturn]] void port_compatibility_blocker(const char*);
static std::mutex trace_lock;
static std::map<uint32_t,unsigned> calls;
static void result(ppu_context* ctx,uint32_t value) { ctx->gpr[3]=(uint64_t)(int64_t)(int32_t)value; }
static void pop_body(ppu_context* ctx)
{
    uint64_t q64=ctx->gpr[3], out64=ctx->gpr[4];
    if(!q64 || !out64) { result(ctx,0x80410911); return; }
    if(q64>0xFFFFFF80ull || out64>0xFFFFFFF0ull) { result(ctx,0x80410902); return; }
    uint32_t q=(uint32_t)q64,out=(uint32_t)out64;
    if((q&127) || (out&15)) { result(ctx,0x80410910); return; }
    // Caller AD215C..AD2170 passes q, out, 0, 1 and treats errors as fatal.
    // Preserve both mode arguments; r6 is the waiting mode on this path.
    if(ctx->gpr[5]>1 || ctx->gpr[6]>1) { result(ctx,0x80410902); return; }
    unsigned n;
    { std::lock_guard<std::mutex> lock(trace_lock); n=++calls[q]; }
    if(n<=2) fprintf(stderr,"[queue] pop q=%08X out=%08X mode5=%llu wait=%llu lr=%08llX\n",
        q,out,(unsigned long long)ctx->gpr[5],(unsigned long long)ctx->gpr[6],(unsigned long long)ctx->lr);
    auto started=GetTickCount64(), logged=started;
    for(;;) {
        spu_lockline_lock();
        uint32_t pop=vm_read32(q),push=vm_read32(q+4),size=vm_read32(q+0x10),depth=vm_read32(q+0x14);
        uint64_t buffer=((uint64_t)vm_read32(q+0x18)<<32)|vm_read32(q+0x1C);
        if(!size || (size&15) || !depth || depth>0x7FFFFFFFu || !buffer || buffer>=0x100000000ull || buffer+(uint64_t)size*depth>0x100000000ull || out64+size>0x100000000ull) {
            spu_lockline_unlock(); result(ctx,0x80410902); return;
        }
        uint64_t modulus=(uint64_t)depth*2;
        bool pending=(int32_t)pop<0 || (int32_t)push<0;
        if(!pending && (pop>=modulus || push>=modulus)) {
            spu_lockline_unlock(); result(ctx,0x8041090F); return;
        }
        if(!pending && pop!=push) {
            uint64_t occupied=(push+modulus-pop)%modulus;
            if(occupied>depth) { spu_lockline_unlock(); result(ctx,0x8041090F); return; }
            if(ctx->gpr[5]) {
                spu_lockline_unlock();
                port_compatibility_blocker("cellSpursQueuePopBody: mode5=1 not verified");
            }
            uint32_t slot=pop%depth;
            memcpy(vm_base+out,vm_base+(uint32_t)buffer+(size_t)slot*size,size);
            uint32_t next=(uint32_t)((pop+1ull)%modulus);
            uint8_t* index=vm_base+q;
            index[0]=(uint8_t)(next>>24); index[1]=(uint8_t)(next>>16);
            index[2]=(uint8_t)(next>>8); index[3]=(uint8_t)next;
            spu_coh_notify_write(q);
            for(uint64_t line=out&~127u;line<out64+size;line+=128) spu_coh_notify_write((uint32_t)line);
            spu_lockline_unlock();
            if(n<=4) fprintf(stderr,"[queue] consumed q=%08X slot=%u index=%u->%u push=%u bytes=%u\n",q,slot,pop,next,push,size);
            result(ctx,0); return;
        }
        spu_lockline_unlock();
        if(!ctx->gpr[6]) { result(ctx,0x80410901); return; }
        auto now=GetTickCount64();
        if(now-logged>=5000) {
            fprintf(stderr,"[queue] waiting for actual producer q=%08X elapsed_ms=%llu\n",q,(unsigned long long)(now-started));
            logged=now;
        }
        Sleep(1);
    }
}
static void lf_push_body(ppu_context* ctx)
{
    // This caller logs an error and continues. Stop here so the missing queue
    // publication cannot be buried under a later resource/allocator failure.
    uint64_t q=ctx->gpr[3],input=ctx->gpr[4];
    fprintf(stderr,"[lfqueue-blocker] push q=%08llX input=%08llX mode=%llu lr=%08llX tid=%llu\n",
        (unsigned long long)q,(unsigned long long)input,(unsigned long long)ctx->gpr[5],
        (unsigned long long)ctx->lr,(unsigned long long)ctx->thread_id);
    if(q>=0x10000 && q<=0xFFFFFF80ull) {
        fprintf(stderr,"[lfqueue-blocker] queue:");
        for(unsigned i=0;i<32;i++) fprintf(stderr," %08X",vm_read32(q+4*i));
        fprintf(stderr,"\n");
    }
    if(input>=0x10000 && input<=0xFFFFFFE0ull) {
        fprintf(stderr,"[lfqueue-blocker] input32:");
        for(unsigned i=0;i<8;i++) fprintf(stderr," %08X",vm_read32(input+4*i));
        fprintf(stderr,"\n");
    }
    port_compatibility_blocker("_cellSpursLFQueuePushBody: guest LF queue publication protocol not implemented");
}
static void lf_initialize(ppu_context* ctx)
{
    uint64_t owner=ctx->gpr[3],q=ctx->gpr[4],buffer=ctx->gpr[5];
    uint32_t size=(uint32_t)ctx->gpr[6],depth=(uint32_t)ctx->gpr[7],direction=(uint32_t)ctx->gpr[8];
    if(!q || (size && !buffer)) { result(ctx,0x80410911); return; }
    if(q>0xFFFFFF80ull || buffer>0xFFFFFFFFull || owner>0xFFFFFFFFull ||
       size>0x4000 || (size&15) || !depth || depth>0x7FFF || direction>3 ||
       buffer+(uint64_t)size*depth>0x100000000ull) { result(ctx,0x80410902); return; }
    if((q&127) || (buffer&15)) { result(ctx,0x80410910); return; }
    if(direction!=3) port_compatibility_blocker("LFQueueInitialize: only observed ANY2ANY fresh initialization verified");
    for(unsigned i=0;i<32;i++) if(vm_read32(q+4*i)) { result(ctx,0x8041090F); return; }
    // CellSyncLFQueue ABI: two groups of four BE16 cursors, not the plain
    // Spurs queue's BE32 pop/push indices. ANY2ANY tags its buffer low bit.
    vm_write32(q+0x10,size); vm_write32(q+0x14,depth);
    vm_write32(q+0x18,0); vm_write32(q+0x1C,(uint32_t)buffer|1u);
    vm_write32(q+0x20,0xFFFF0000u); vm_write32(q+0x24,3);
    vm_write32(q+0x28,0xFFFFFFFFu);
    vm_write32(q+0x30,0xFFFF0000u); vm_write32(q+0x50,0xFFFF0000u);
    vm_write32(q+0x70,0); vm_write32(q+0x74,(uint32_t)owner);
    fprintf(stderr,"[lfqueue] fresh ANY2ANY q=%08X buffer=%08X size=%u depth=%u owner=%08X\n",
        (uint32_t)q,(uint32_t)buffer,size,depth,(uint32_t)owner);
    result(ctx,0);
}
extern "C" void port_register_queue_api()
{
    ps3_hle_register_ctx(ps3_compute_nid("cellSpursQueuePopBody"),"cellSpursQueuePopBody",pop_body);
    ps3_hle_register_ctx(ps3_compute_nid("_cellSpursLFQueuePushBody"),"_cellSpursLFQueuePushBody",lf_push_body);
    ps3_hle_register_ctx(ps3_compute_nid("_cellSpursLFQueueInitialize"),"_cellSpursLFQueueInitialize",lf_initialize);
}
