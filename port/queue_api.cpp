#include "ppu_recomp.h"
#include "ps3emu/nid.h"
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <cstring>
#include "win32_compat.h"
#include "spu_context.h"
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
struct LFConsumer { uint32_t taskset, task; };
static std::map<uint32_t,LFConsumer> lf_consumers;
static uint16_t lf_read16(uint32_t ea) { return (uint16_t)((vm_base[ea]<<8)|vm_base[ea+1]); }
static void lf_write16(uint32_t ea,uint16_t v) { vm_base[ea]=(uint8_t)(v>>8); vm_base[ea+1]=(uint8_t)v; }
// The SDK currently assigns workload 0 to every taskset. Bind the observed
// image-42 service to its enqueue-time queue argument, never the global last
// taskset. A notification names a waiter from the guest LF ring.
extern "C" void port_lf_register_consumer(uint32_t q,uint32_t taskset,uint32_t task)
{
    std::lock_guard<std::mutex> lock(trace_lock);
    auto it=lf_consumers.find(q);
    if(it!=lf_consumers.end() && (it->second.taskset!=taskset || it->second.task!=task))
        port_compatibility_blocker("LFQueue: ambiguous consumer identity");
    lf_consumers[q]={taskset,task};
}
extern "C" void spu_taskset_signal_task(uint32_t,uint32_t);
extern "C" void port_lf_atomic_trace(spu_context* ctx,uint32_t ea,const uint8_t* line)
{
    static const bool enabled=std::getenv("PORT_LF_TRACE")!=nullptr;
    if(!enabled || ctx->image_id!=42) return;
    std::lock_guard<std::mutex> lock(trace_lock);
    static unsigned records=0;
    if(records++>=128) return;
    fprintf(stderr,"[lf-spu-commit] ea=%08X lr=%05X words:",ea,ctx->gpr[0]._u32[0]);
    for(unsigned k=0;k<8;k++) {
        auto* p=line+4*k;
        fprintf(stderr," %08X",((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3]);
    }
    fprintf(stderr,"\n");
}
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
    uint64_t q64=ctx->gpr[3],input64=ctx->gpr[4];
    if(!q64 || !input64) { result(ctx,0x80410911); return; }
    if(q64>0xFFFFFF80ull || input64>0xFFFFFFFFull) { result(ctx,0x80410902); return; }
    uint32_t q=(uint32_t)q64,input=(uint32_t)input64;
    if((q&127) || (input&15)) { result(ctx,0x80410910); return; }
    if(ctx->gpr[5]!=3) port_compatibility_blocker("LFQueuePushBody: only observed mode 3 supported");
    unsigned n;
    { std::lock_guard<std::mutex> lock(trace_lock); n=++calls[q]; }
    auto started=GetTickCount64(),logged=started;
    for(;;) {
        spu_lockline_lock();
        uint32_t size=vm_read32(q+0x10),depth=vm_read32(q+0x14);
        uint64_t tagged=((uint64_t)vm_read32(q+0x18)<<32)|vm_read32(q+0x1C);
        uint64_t buffer=tagged&~1ull;
        if(!size || size>0x4000 || (size&15) || !depth || depth>0x7FFF ||
           !(tagged&1) || !buffer || buffer+(uint64_t)size*depth>0x100000000ull ||
           input64+size>0x100000000ull) {
            spu_lockline_unlock(); result(ctx,0x80410902); return;
        }
        if(vm_read32(q+0x24)!=3) {
            spu_lockline_unlock(); port_compatibility_blocker("LFQueuePushBody: non-ANY2ANY queue");
        }
        uint32_t pop=lf_read16(q),push=lf_read16(q+8),reserved=lf_read16(q+14),mod=depth*2;
        if(pop>=mod || push>=mod || reserved>=mod) {
            spu_lockline_unlock(); result(ctx,0x8041090F); return;
        }
        // One lock covers reservation, payload and publication. There can be
        // concurrent PPU producers, but a split-phase SPU producer is not yet
        // supported: never discard its reservation/completion bitmap.
        if(reserved!=push || lf_read16(q+10)) {
            spu_lockline_unlock(); port_compatibility_blocker("LFQueuePushBody: outstanding split-phase producer");
        }
        uint32_t used=(push+mod-pop)%mod;
        if(used>depth) { spu_lockline_unlock(); result(ctx,0x8041090F); return; }
        if(used<depth) {
            uint16_t control=lf_read16(q+4);
            unsigned queued=control&31,issued=(control>>10)&31,ack=(control>>5)&31;
            if(queued>=30 || issued>=30 || ack>=30) {
                spu_lockline_unlock(); result(ctx,0x8041090F); return;
            }
            unsigned pending=(queued+30-issued)%30;
            LFConsumer consumer{};
            // Signal at most one registered waiter for this newly published
            // entry. Ring indices wrap at 30; its 15 token slots are reused.
            if(pending) {
                uint16_t token=lf_read16(q+0x32+2*(issued%15));
                { std::lock_guard<std::mutex> lock(trace_lock);
                  auto it=lf_consumers.find(q); if(it!=lf_consumers.end()) consumer=it->second; }
                if(pending!=1 || !consumer.taskset || token!=consumer.task) {
                    spu_lockline_unlock(); port_compatibility_blocker("LFQueuePushBody: unsupported waiter token/owner");
                }
            }
            uint32_t slot=push%depth,next=(push+1)%mod;
            lf_write16(q+14,(uint16_t)next);
            memmove(vm_base+(uint32_t)buffer+(size_t)slot*size,vm_base+input,size);
            for(uint64_t line=(buffer+(uint64_t)slot*size)&~127ull;
                line<buffer+(uint64_t)(slot+1)*size;line+=128) spu_coh_notify_write((uint32_t)line);
            lf_write16(q+8,(uint16_t)next);
            if(pending) lf_write16(q+4,(uint16_t)((control&~0x7C00u)|(((issued+1)%30)<<10)));
            spu_coh_notify_write(q);
            spu_lockline_unlock();
            if(pending) spu_taskset_signal_task(consumer.taskset,consumer.task);
            if(n<=8) fprintf(stderr,"[lfqueue] published q=%08X slot=%u push=%u->%u pop=%u bytes=%u waiter=%u taskset=%08X\n",
                q,slot,push,next,pop,size,pending?1u:0u,consumer.taskset);
            result(ctx,0); return;
        }
        spu_lockline_unlock();
        auto now=GetTickCount64();
        if(now-logged>=5000) { fprintf(stderr,"[lfqueue] full q=%08X elapsed_ms=%llu\n",q,(unsigned long long)(now-started)); logged=now; }
        Sleep(1);
    }
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
