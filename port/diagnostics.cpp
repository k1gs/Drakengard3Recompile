#include "ppu_recomp.h"
#include "win32_compat.h"
#include <cstdio>
#include <cstdlib>
#include "spu_context.h"
#include <atomic>
#include <thread>
#ifdef _WIN32
#include <dbghelp.h>
#endif

extern "C" PPU_THREAD_LOCAL ppu_context* g_active_ctx;
extern "C" uint32_t vm_read32(uint64_t);
extern "C" uint64_t vm_read64(uint64_t);
extern "C" uint32_t ppu_hle_inject_base;
extern "C" uint32_t port_gcm_io_ea(uint32_t);
extern "C" uint32_t ppu_prof_resolve_host(void*);
extern "C" void ppu_dump_guest_stack(ppu_context*,const char*);
extern "C" void port_resource_atomic(unsigned long long ea,unsigned int old,unsigned int val,void* opaque,void* caller)
{
    static const bool enabled=std::getenv("PORT_RESOURCE_TRACE")!=nullptr;
    if(!enabled || ea!=0x01AC3218 || !old || val) return;
    auto* ctx=static_cast<ppu_context*>(opaque);
    fprintf(stderr,"[resource-atomic] ea=%08llX expected=%08X new=%08X fn=%08X\n",ea,old,val,ppu_prof_resolve_host(caller));
    if(ctx) ppu_dump_guest_stack(ctx,"resource-atomic");
}

extern "C" void port_trace_allocation_call(void* opaque,void* caller)
{
    static const bool enabled=std::getenv("PORT_ALLOC_TRACE")!=nullptr;
    if(!enabled) return;
    auto* ctx=static_cast<ppu_context*>(opaque);
    uint32_t target=(uint32_t)ctx->ctr;
    if(ctx->lr==0x00CB5BA0) {
        static std::atomic<unsigned> queries{0};
        if(queries.fetch_add(1)<4) {
            uint32_t object=(uint32_t)ctx->gpr[22];
            if(object>=0x10000 && object<=0xFFFFFC00u) {
                uint32_t resource=vm_read32(object+0x3B8);
                if(resource>=0x10000 && resource<=0xFFFFFF00u) {
                    uint32_t handle=vm_read32(resource+0xBC);
                    uint32_t table=vm_read32(0x01981090-0x3F7C);
                    uint32_t offset=(handle<<2)&0x003FFFFCu;
                    fprintf(stderr,"[resource-query] receiver=%08llX method=%08X object=%08X resource=%08X handle=%08X type=%u table=%08X offset=%08X entry=%08X tid=%llu\n",
                        (unsigned long long)ctx->gpr[3],target,
                        object,resource,handle,(handle>>20)&63,table,offset,
                        table>=0x10000 && (uint64_t)table+offset<=0xFFFFFFFCull?vm_read32(table+offset):0,
                        (unsigned long long)ctx->thread_id);
                }
            }
        }
    }
    // Known allocator entries from the failing native saved stack.
    if(target!=0x0141A210 && target!=0x00E062F0 && target!=0x012EFED8) return;
    static std::atomic<unsigned> records{0};
    if((uint32_t)ctx->gpr[4]<0x100000 || records.fetch_add(1)>=64) return;
    fprintf(stderr,"[alloc-call] target=%08X from=%08X tid=%llu lr=%08llX r3=%08llX size=%08llX r5=%08llX r6=%08llX sp=%08llX\n",
        target,ppu_prof_resolve_host(caller),(unsigned long long)ctx->thread_id,
        (unsigned long long)ctx->lr,(unsigned long long)ctx->gpr[3],
        (unsigned long long)ctx->gpr[4],(unsigned long long)ctx->gpr[5],
        (unsigned long long)ctx->gpr[6],(unsigned long long)ctx->gpr[1]);
    if((uint32_t)ctx->gpr[4]>=0x4000000) {
        ppu_dump_guest_stack(ctx,"large-allocation-entry");
#ifdef _WIN32
        void* frames[32];
        unsigned n=RtlCaptureStackBackTrace(0,32,frames,nullptr);
        fprintf(stderr,"[alloc-host-chain]");
        uintptr_t base=(uintptr_t)GetModuleHandleW(nullptr);
        for(unsigned i=0;i<n;i++) fprintf(stderr," %08X@%llX",ppu_prof_resolve_host(frames[i]),
            (unsigned long long)((uintptr_t)frames[i]-base));
        fprintf(stderr,"\n");
#endif
    }
}

extern "C" void port_trace_ppu_context(void* opaque)
{
    auto* ctx=static_cast<ppu_context*>(opaque);
    if(ctx->lr!=0x008B0D18 && ctx->thread_id!=20) return;
    if(ctx->lr==0x008B0D18) {
    uint64_t object=ctx->gpr[30],global=ctx->gpr[28];
    if(object<0x10000 || object>0xFFFFFF00ull || global<0x10000 || global>0xFFFFFF00ull) return;
    uint32_t label=vm_read32(object+0x50);
    if(label<0x10000 || label>0xFFFFFFFCu) return;
    fprintf(stderr,"[render-fence] tid=%llu object=%08llX label=%08X value=%08X target=%016llX threshold=%016llX last=%016llX\n",
        (unsigned long long)ctx->thread_id,(unsigned long long)object,label,vm_read32(label),
        (unsigned long long)ctx->gpr[29],(unsigned long long)ctx->gpr[27],(unsigned long long)vm_read64(global-0x5DB0));
    }
    uint32_t put=vm_read32(ppu_hle_inject_base+0x2000),get=vm_read32(ppu_hle_inject_base+0x2004);
    fprintf(stderr,"[render-fifo] put=%08X get=%08X ref=%08X\n",put,get,vm_read32(ppu_hle_inject_base+0x2008));
    const char* directory=std::getenv("PORT_FIFO_DUMP_DIR");
    if(directory && put>=512 && put<0xFFFFFF00u) {
        // Use the active runtime mapping; the guest table may not be populated.
        for(unsigned region=0;region<2;region++) {
        uint32_t start=region?get:put-512;
        if(start>0xFFFFFD00u) continue;
        uint8_t bytes[576]; bool mapped=true;
        for(unsigned i=0;i<sizeof bytes;i+=4) {
            uint32_t io=start+i,ea=port_gcm_io_ea(io);
            if(!ea) { mapped=false; break; }
            uint32_t word=vm_read32(ea);
            bytes[i]=(uint8_t)(word>>24); bytes[i+1]=(uint8_t)(word>>16);
            bytes[i+2]=(uint8_t)(word>>8); bytes[i+3]=(uint8_t)word;
        }
        if(mapped) {
            static std::atomic<unsigned> sequence{0}; char path[1024];
            int n=snprintf(path,sizeof path,"%s/fifo_%u_io%08X_put%08X_get%08X.bin",directory,sequence.fetch_add(1),start,put,get);
            if(n>0 && (size_t)n<sizeof path) {
                FILE* file=fopen(path,"wb");
                if(file) { size_t written=fwrite(bytes,1,sizeof bytes,file); fclose(file);
                    fprintf(stderr,"[render-fifo] snapshot io=%08X ea=%08X bytes=%zu -> %s\n",start,port_gcm_io_ea(start),written,path); }
            }
        }
        }
    }
}

extern "C" void port_event_wait_trace(void* opaque)
{
    auto* ctx=static_cast<ppu_context*>(opaque);
    static const bool enabled=std::getenv("PORT_EVENT_TRACE")!=nullptr;
    if(!enabled || ctx->lr!=0x008EFE08) return;
    static std::atomic<unsigned> count{0};
    if(count.fetch_add(1)>=4) return;
    uint64_t object=ctx->gpr[28];
    if(object<0x10000 || object>0xFFFFFF00ull) return;
    uint32_t slot=vm_read32(object+8);
    if(slot>1) return;
    uint32_t buffer=vm_read32(object+slot*4);
    if(buffer<0x10000 || buffer>0xFFFFFF00u) return;
    uint32_t vtable=vm_read32(buffer);
    if(vtable<0x10000 || vtable>0xFFFFFF00u) return;
    fprintf(stderr,"[event-trace] q=%u caller=%08llX object=%08llX slot=%u buffer=%08X vtable=%08X poll_opd=%08X signal_opd=%08X fields:",
        (uint32_t)ctx->gpr[3],(unsigned long long)ctx->lr,(unsigned long long)object,slot,buffer,vtable,
        vm_read32(vtable+0x18),vm_read32(vtable+0xC));
    for(unsigned o=0;o<64;o+=4) fprintf(stderr," %08X",vm_read32(buffer+o));
    fprintf(stderr,"\n");
}

static void guest_context()
{
    auto* ctx = g_active_ctx;
    if (!ctx) return;
    fprintf(stderr, "[guest] cia=%08llX lr=%08llX ctr=%08llX tid=%llu\n",
            (unsigned long long)ctx->cia, (unsigned long long)ctx->lr,
            (unsigned long long)ctx->ctr, (unsigned long long)ctx->thread_id);
    for (int i = 0; i < 32; i += 4)
        fprintf(stderr, "[guest] r%d=%016llX r%d=%016llX r%d=%016llX r%d=%016llX\n",
                i, (unsigned long long)ctx->gpr[i], i+1, (unsigned long long)ctx->gpr[i+1],
                i+2, (unsigned long long)ctx->gpr[i+2], i+3, (unsigned long long)ctx->gpr[i+3]);
}

extern "C" [[noreturn]] void port_unimplemented_instruction(unsigned long long function, const char* detail)
{
    fprintf(stderr, "[lift-blocker] function=%08llX instruction=%s\n", function, detail);
    guest_context();
    fflush(stderr);
    std::exit(3);
}

extern "C" void port_spu_unsupported(uint32_t pc, const char* mnemonic)
{
    fprintf(stderr, "[spu-lift-blocker] ls_pc=%05X instruction=%s\n", pc, mnemonic);
    fflush(stderr);
    std::exit(4);
}

extern "C" [[noreturn]] void port_resource_blocker(const char* path)
{
    fprintf(stderr, "[resource-blocker] encrypted NPD data is not usable as plaintext: %s\n", path);
    fprintf(stderr, "[resource-blocker] A supported, authorized plaintext resource is required; original data was not modified.\n");
    guest_context();
    fflush(stderr);
    std::exit(5);
}

extern "C" [[noreturn]] void port_compatibility_blocker(const char* api)
{
    fprintf(stderr, "[compatibility-blocker] %s\n", api);
    guest_context();
    fflush(stderr);
    std::exit(6);
}

extern "C" void port_spu_job_missing(uint64_t fingerprint, uint32_t size, uint32_t job)
{
    fprintf(stderr,"[spu-job-blocker] unregistered raw SPURS job fp=%016llX size=%u descriptor=%08X\n",
            (unsigned long long)fingerprint,size,job);
    fflush(stderr);
    std::exit(7);
}

extern "C" void port_task_dispatch_blocker(const char* reason)
{
    fprintf(stderr,"[spu-task-blocker] %s\n",reason);
    fflush(stderr);
    std::exit(9);
}

extern "C" void port_spu_branch_missing(spu_context* ctx)
{
    fprintf(stderr,"[spu-dispatch-blocker] image=%d pc=%05X lr=%05X; execution incomplete\n",
            ctx->image_id,ctx->pc,ctx->gpr[0]._u32[0] & SPU_LS_MASK);
    const char* directory=std::getenv("SPU_DUMP_MISS");
    if(directory && *directory) {
        char path[1024];
        int length=snprintf(path,sizeof path,"%s/spu_missing_img%d_pc%05X.ls",directory,ctx->image_id,ctx->pc);
        if(length>0 && (size_t)length<sizeof path) {
            FILE* file=fopen(path,"wb");
            if(file) {
                size_t written=fwrite(ctx->ls,1,SPU_LS_SIZE,file);
                fclose(file);
                fprintf(stderr,"[spu-dispatch-blocker] LS capture %zu bytes -> %s\n",written,path);
            }
        }
        length=snprintf(path,sizeof path,"%s/spu_missing_img%d_pc%05X.registers.json",directory,ctx->image_id,ctx->pc);
        if(length>0 && (size_t)length<sizeof path) {
            FILE* file=fopen(path,"wb");
            if(file) {
                fprintf(file,"{\"image\":%d,\"pc\":%u,\"status\":%u,\"stop_code\":%u,\"gpr\":[",ctx->image_id,ctx->pc,ctx->status,ctx->stop_code);
                for(unsigned r=0;r<128;r++) fprintf(file,"%s[%u,%u,%u,%u]",r?",":"",ctx->gpr[r]._u32[0],ctx->gpr[r]._u32[1],ctx->gpr[r]._u32[2],ctx->gpr[r]._u32[3]);
                fprintf(file,"]}\n"); fclose(file);
            }
        }
    }
    fflush(stderr);
    std::exit(8);
}

#ifdef _WIN32
static LONG WINAPI crash_filter(EXCEPTION_POINTERS* ep)
{
    auto base = (uintptr_t)GetModuleHandleW(nullptr);
    fprintf(stderr, "[host-crash] exception=%08lX address=%p rva=%llX\n",
            ep->ExceptionRecord->ExceptionCode, ep->ExceptionRecord->ExceptionAddress,
            (unsigned long long)((uintptr_t)ep->ExceptionRecord->ExceptionAddress-base));
    guest_context();
    auto file = CreateFileW(L"native-crash.dmp", GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION info{GetCurrentThreadId(), ep, FALSE};
        BOOL ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, MiniDumpNormal, &info, nullptr, nullptr);
        fprintf(stderr, "[host-crash] minidump=%s\n", ok ? "native-crash.dmp" : "failed");
        CloseHandle(file);
    }
    fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

extern "C" void port_install_diagnostics()
{
#ifdef _WIN32
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    SetUnhandledExceptionFilter(crash_filter);
#endif
    if(std::getenv("PORT_PPU_SAMPLE")) {
        std::thread([] {
            extern void port_sample_ppu_threads();
            for(unsigned i=0;i<3;i++) {
                Sleep(30000);
                port_sample_ppu_threads();
            }
        }).detach();
    }
}

extern "C" void ppu_report_guest_lrs();
void port_sample_ppu_threads()
{
    fprintf(stderr,"[port-ppu-sample] best-effort last direct-call sites\n");
    ppu_report_guest_lrs();
}
