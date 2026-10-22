#include "ppu_recomp.h"
#include "win32_compat.h"
#include <cstdio>
#include <cstdlib>
#include "spu_context.h"
#include <atomic>
#include <thread>
#include <memory>
#include <cstring>
#ifdef _WIN32
#include <dbghelp.h>
#include <tlhelp32.h>
#endif

extern "C" PPU_THREAD_LOCAL ppu_context* g_active_ctx;
extern "C" uint8_t* vm_base;
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

namespace {
struct TaskDmaRecord {
    uint32_t ea,lsa,size,pc,lr;
    uint8_t bytes[16384];
};
struct TaskDmaHistory {
    unsigned count=0;
    TaskDmaRecord records[32];
};
thread_local std::unique_ptr<TaskDmaHistory> task_dma_history;
thread_local spu_context* task_dma_context;
}

extern "C" void port_task_dma_context(spu_context* ctx)
{
    static const bool enabled=std::getenv("PORT_TASK_WRITE_WATCH")!=nullptr;
    if(enabled) task_dma_context=ctx;
}

extern "C" void port_task_dma_trace(spu_context* ctx,uint32_t ea,const uint8_t* ls,uint32_t size)
{
    static const bool enabled=std::getenv("PORT_TASK_RESULT_TRACE")!=nullptr;
    if(!enabled || (ctx->image_id!=55 && ctx->image_id!=56)) return;
    if(ea==0x800) {
        uint32_t base=vm_read32(0x01AC39B8);
        fprintf(stderr,"[task-bad-dma] global=%08X field=%08X\n",base,
            base>=0x10000 && base<0xFFFFFF80?vm_read32(base+0x24):0);
    }
    if(!task_dma_history) task_dma_history=std::make_unique<TaskDmaHistory>();
    auto& history=*task_dma_history;
    auto& record=history.records[history.count++%32];
    record.ea=ea;record.lsa=(uint32_t)(ls-ctx->ls);record.size=size;
    record.pc=ctx->pc;record.lr=ctx->gpr[0]._u32[0];
    auto copied=size<sizeof record.bytes?size:sizeof record.bytes;
    std::memcpy(record.bytes,ls,copied);
}

extern "C" void port_spu_branch_missing(spu_context* ctx)
{
    fprintf(stderr,"[spu-dispatch-blocker] image=%d pc=%05X lr=%05X; execution incomplete\n",
            ctx->image_id,ctx->pc,ctx->gpr[0]._u32[0] & SPU_LS_MASK);
    const char* directory=std::getenv("SPU_DUMP_MISS");
    if(directory && *directory) {
        char path[1024];
        if(ctx->image_id==55 && task_dma_history) {
            int n=snprintf(path,sizeof path,"%s/task-dma-history.bin",directory);
            if(n>0 && (size_t)n<sizeof path) {
                FILE* file=fopen(path,"wb");
                if(file) {
                    auto& history=*task_dma_history;
                    unsigned count=history.count<32?history.count:32;
                    uint32_t header[3]={0x31414D44u,count,16384}; // DMA1, host little endian
                    fwrite(header,sizeof header,1,file);
                    for(unsigned i=history.count-count;i<history.count;i++) {
                        auto& record=history.records[i%32];
                        fwrite(&record.ea,5*sizeof(uint32_t),1,file);
                        auto size=record.size<sizeof record.bytes?record.size:sizeof record.bytes;
                        fwrite(record.bytes,1,size,file);
                    }
                    fclose(file);
                }
            }
        }
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
static thread_local uint32_t task_watch_ea;
static std::atomic<unsigned> task_watch_hits{0};
static std::atomic<uint32_t> task_watch_base{0};
static constexpr DWORD task_watch_exception=0x406D3A55;
static LONG CALLBACK task_write_watch(EXCEPTION_POINTERS* ep)
{
    auto* cpu=ep->ContextRecord;
    if(ep->ExceptionRecord->ExceptionCode==task_watch_exception) {
        bool ours=(cpu->Dr7&0xFF)==1 && ((cpu->Dr7>>16)&15)==13;
        if((cpu->Dr7&0xFF) && !ours) { task_watch_ea=0;return EXCEPTION_CONTINUE_EXECUTION; }
        if(!task_watch_ea) { cpu->Dr7=0;cpu->Dr0=0;cpu->Dr6=0;return EXCEPTION_CONTINUE_EXECUTION; }
        cpu->ContextFlags|=CONTEXT_DEBUG_REGISTERS;
        cpu->Dr0=(DWORD64)(vm_base+task_watch_ea);
        cpu->Dr7=0xD0001; // DR0: local, write-only, four bytes.
        cpu->Dr6=0;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if(ep->ExceptionRecord->ExceptionCode!=EXCEPTION_SINGLE_STEP || !(cpu->Dr6&1) || ((cpu->Dr7>>16)&15)!=13)
        return EXCEPTION_CONTINUE_SEARCH;
    uint64_t offset=cpu->Dr0-(DWORD64)vm_base;
    if(offset>0xFFFFFFFC) return EXCEPTION_CONTINUE_SEARCH;
    uint32_t written_ea=(uint32_t)offset;
    cpu->Dr6=0;
    if(vm_read32(written_ea)!=0x800) return EXCEPTION_CONTINUE_EXECUTION;
    cpu->Dr7=0;cpu->Dr0=0;
    ++task_watch_hits;
    fprintf(stderr,"[task-hardware-write] ea=%08X value=800 rip=%p fn=%08X\n",
        written_ea,(void*)cpu->Rip,ppu_prof_resolve_host((void*)(cpu->Rip-1)));
    if(task_dma_context) {
        auto* ctx=task_dma_context;
        fprintf(stderr,"[task-hardware-spu] image=%d pc=%05X lr=%05X lsa=%05X eal=%08X size=%X\n",
            ctx->image_id,ctx->pc,ctx->gpr[0]._u32[0],ctx->mfc_lsa,ctx->mfc_eal,ctx->mfc_size);
        char name[80];snprintf(name,sizeof name,"task-writer%d-output.bin",ctx->image_id);
        FILE* f=fopen(name,"wb");
        if(f) { fwrite(ctx->gpr,sizeof ctx->gpr,1,f);fwrite(ctx->ls,1,SPU_LS_SIZE,f);fclose(f); }
        if(task_dma_history) {
            snprintf(name,sizeof name,"task-writer%d-dma.bin",ctx->image_id);f=fopen(name,"wb");
            if(f) {
                auto& h=*task_dma_history;unsigned count=h.count<32?h.count:32;
                uint32_t header[3]={0x31414D44u,count,16384};fwrite(header,sizeof header,1,f);
                for(unsigned i=h.count-count;i<h.count;i++) {
                    auto& r=h.records[i%32];fwrite(&r.ea,5*sizeof(uint32_t),1,f);
                    fwrite(r.bytes,1,r.size<16384?r.size:16384,f);
                }
                fclose(f);
            }
        }
    }
    if(g_active_ctx) {
        ppu_dump_guest_stack(g_active_ctx,"task-hardware-write");
        fprintf(stderr,"[task-hardware-write] gpr:");
        for(unsigned r=0;r<32;r++) fprintf(stderr," r%u=%llX",r,(unsigned long long)g_active_ctx->gpr[r]);
        fprintf(stderr,"\n");
    }
    task_watch_ea=0;
    return EXCEPTION_CONTINUE_EXECUTION;
}

extern "C" void port_task_arm_write_watch(uint64_t address,uint32_t value)
{
    static const bool enabled=std::getenv("PORT_TASK_WRITE_WATCH")!=nullptr;
    if(!enabled || address!=0x01AC39B8 || value<0x40000000 || value>=0xBFFFFF80) return;
    if(task_watch_base.exchange(value)==value) return;
    task_watch_ea=value+0x24;
    RaiseException(task_watch_exception,0,0,nullptr);
    // The publishing PPU thread can hand filling the block to another worker.
    HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD,0);
    if(snapshot==INVALID_HANDLE_VALUE) return;
    THREADENTRY32 entry{};entry.dwSize=sizeof entry;
    if(Thread32First(snapshot,&entry)) do {
        if(entry.th32OwnerProcessID!=GetCurrentProcessId() || entry.th32ThreadID==GetCurrentThreadId()) continue;
        HANDLE thread=OpenThread(THREAD_SUSPEND_RESUME|THREAD_GET_CONTEXT|THREAD_SET_CONTEXT,FALSE,entry.th32ThreadID);
        if(!thread) continue;
        if(SuspendThread(thread)!=(DWORD)-1) {
            CONTEXT cpu{};cpu.ContextFlags=CONTEXT_DEBUG_REGISTERS;
            if(GetThreadContext(thread,&cpu)) {
                bool ours=(cpu.Dr7&0xFF)==1 && ((cpu.Dr7>>16)&15)==13;
                if(!(cpu.Dr7&0xFF) || ours) {
                    cpu.Dr0=(DWORD64)(vm_base+value+0x24);cpu.Dr7=0xD0001;cpu.Dr6=0;
                    SetThreadContext(thread,&cpu);
                }
            }
            ResumeThread(thread);
        }
        CloseHandle(thread);
    } while(Thread32Next(snapshot,&entry));
    CloseHandle(snapshot);
    static std::atomic<unsigned> armed{0};
    if(armed.fetch_add(1)<16) fprintf(stderr,"[task-hardware-arm] ea=%08X\n",value+0x24);
}

extern "C" void port_task_arm_current_watch()
{
    static const bool enabled=std::getenv("PORT_TASK_WRITE_WATCH")!=nullptr;
    if(!enabled) return;
    uint32_t base=task_watch_base.load();
    if(!base) return;
    task_watch_ea=base+0x24;
    RaiseException(task_watch_exception,0,0,nullptr);
}

extern "C" int port_check_task_write_watch()
{
    auto handler=AddVectoredExceptionHandler(1,task_write_watch);
    if(!handler) return 2;
    auto* field=reinterpret_cast<volatile uint32_t*>(vm_base+0x60000024);
    *field=0;
    task_watch_hits=0;task_watch_ea=0x60000024;
    RaiseException(task_watch_exception,0,0,nullptr);
    *field=0x00080000; // Big-endian guest value 0x800, written without vm_write32.
    bool ok=task_watch_hits==1 && task_watch_ea==0 && vm_read32(0x60000024)==0x800;
    *field=0;task_watch_hits=0;
    std::atomic<bool> go{false};
    std::thread writer([&] { while(!go.load()) Sleep(1);*field=0x00080000; });
    _putenv_s("PORT_TASK_WRITE_WATCH","1");
    port_task_arm_write_watch(0x01AC39B8,0x60000000);
    go.store(true);writer.join();
    ok=ok && task_watch_hits==1;
    task_watch_ea=0;RaiseException(task_watch_exception,0,0,nullptr);
    task_watch_base=0;
    RemoveVectoredExceptionHandler(handler);
    fprintf(stderr,"[compat-check] hardware task write watch: %s\n",ok?"PASS":"FAIL");
    return ok?0:1;
}

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
    if(std::getenv("PORT_TASK_WRITE_WATCH")) AddVectoredExceptionHandler(1,task_write_watch);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    SetUnhandledExceptionFilter(crash_filter);
#endif
    if(std::getenv("PORT_PPU_SAMPLE")) {
        std::thread([] {
            extern void port_sample_ppu_threads();
            for(unsigned i=0;i<10;i++) {
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
