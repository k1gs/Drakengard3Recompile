#include "ppu_recomp.h"
#include "ps3emu/nid.h"
#include <cstdio>
#include <cstring>
#include <array>
#include <thread>
#include <chrono>
#include <memory>
#include <vector>
#include <atomic>
#include "spu_context.h"
#include "spu_workload.h"
#include "spu_coherency.h"

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
extern "C" void spu_begin_image(int);
extern "C" void spu_0001_at_01799780_spu_recomp_register();
extern "C" void spu_0001_at_01799780_spu_func_00009CB0(spu_context*);
extern "C" void spu_0001_at_01799780_spu_func_00009AD8(spu_context*);
extern "C" void port_lf_register_consumer(uint32_t,uint32_t,uint32_t);
extern "C" uint64_t spurs_pm_build_context(uint8_t*,uint32_t,uint32_t,uint32_t,uint32_t);
extern "C" void spurs_taskset_init(uint32_t,uint32_t,uint64_t,uint32_t,uint32_t,uint32_t,uint32_t);
extern "C" void spurs_taskset_add_task(uint32_t,uint32_t,uint64_t,uint64_t,const uint32_t*,const uint32_t*);
extern "C" void lv2_init_syscalls();
extern "C" uint32_t sys_event_queue_create_direct(uint64_t,int32_t);
extern "C" int64_t sys_event_queue_tryreceive(ppu_context*);
extern "C" int32_t cellSpursAttachLv2EventQueue(void*,uint32_t,uint8_t*,int32_t);
extern "C" int32_t cellSpursCreateJobChain(uint64_t,uint64_t,uint64_t,uint32_t,uint32_t,uint64_t,uint32_t,uint32_t);
extern "C" int32_t cellSpursRunJobChain(uint64_t);
extern "C" int32_t cellSpursJoinJobChain(uint64_t);
extern "C" void spu_wrch(spu_context*,uint32_t,u128);
extern "C" void vm_write64(uint64_t,uint64_t);
static std::atomic<unsigned> event_jobs{0};
extern "C" void (*port_jc_end_hook)(uint32_t,uint32_t);
static std::atomic<unsigned> resume_jobs[4]{};
static std::atomic<bool> resume_end_seen{false},resume_end_release{false};
static void resume_test_job(spu_context* ctx) { resume_jobs[ctx->image_id-204]++; }
static void resume_test_end(uint32_t chain,uint32_t)
{
    if(chain!=0x65100 || resume_end_seen.exchange(true)) return;
    while(!resume_end_release.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
}
extern "C" int port_check_jobchain_resume()
{
    lv2_init_syscalls();
    for(unsigned k=0;k<4;k++) {
        uint32_t binary=0x66000+k*0x100,desc=0x64000+k*0x100;
        memset(vm_base+binary,(int)(20+k),16);memset(vm_base+desc,0,256);
        spu_workload_register_img(spu_workload_fingerprint(vm_base+binary,16),resume_test_job,204+k,"chain-resume");
        vm_write64(desc,binary);vm_write32(desc+8,0x00010000);
        resume_jobs[k]=0;
    }
    bool ok=cellSpursCreateJobChain(0x58000,0x65000,0x64800,256,1,0,1,0)==0;
    for(unsigned k=0;k<3;k++) {
        vm_write64(0x64800+8*(k+1),127);vm_write64(0x64800+8*k,0x64000+k*0x100);
        if(cellSpursRunJobChain(0x65000) || cellSpursJoinJobChain(0x65000)) ok=false;
    }
    vm_write64(0x64A00,0x64300);vm_write64(0x64A08,127);
    vm_write64(0x64818,0x64A01); // RESET_PC to another segment, then one new JOB
    if(cellSpursRunJobChain(0x65000) || cellSpursJoinJobChain(0x65000)) ok=false;
    for(unsigned k=0;k<4;k++) if(resume_jobs[k]!=1) ok=false;
    fprintf(stderr,"[compat-check] chain append/branch exact executions=%u/%u/%u/%u %s\n",
        resume_jobs[0].load(),resume_jobs[1].load(),resume_jobs[2].load(),resume_jobs[3].load(),ok?"PASS":"FAIL");
    for(auto& count:resume_jobs) count=0;
    vm_write64(0x64900,0x64000);vm_write64(0x64908,127);
    if(cellSpursCreateJobChain(0x58000,0x65100,0x64900,256,1,0,1,0)) return 2;
    resume_end_seen=false;resume_end_release=false;port_jc_end_hook=resume_test_end;
    cellSpursRunJobChain(0x65100);
    auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while(!resume_end_seen && std::chrono::steady_clock::now()<deadline) std::this_thread::yield();
    bool arrived=resume_end_seen.load();
    vm_write64(0x64910,127);vm_write64(0x64908,0x64100);
    cellSpursRunJobChain(0x65100);resume_end_release=true;cellSpursJoinJobChain(0x65100);
    port_jc_end_hook=nullptr;
    bool wake=arrived && resume_jobs[0]==1 && resume_jobs[1]==1;
    fprintf(stderr,"[compat-check] chain END/publication race executions=%u/%u %s\n",
        resume_jobs[0].load(),resume_jobs[1].load(),wake?"PASS":"FAIL");
    return ok && wake?0:1;
}
static void event_test_job(spu_context* ctx)
{
    if(ctx->image_id==203) {
        u128 value{};value._u32[0]=0x12345678;
        spu_wrch(ctx,SPU_WrOutMbox,value);
        value._u32[0]=(4u<<24)|7u;
        spu_wrch(ctx,SPU_WrOutIntrMbox,value);
    }
    event_jobs++;
}
extern "C" int port_check_spurs_events()
{
    lv2_init_syscalls();
    uint32_t queues[2]={sys_event_queue_create_direct(0,16),sys_event_queue_create_direct(0,16)};
    bool ok=queues[0] && queues[1];
    for(unsigned i=0;i<2;i++) {
        vm_base[0x50000+i]=(uint8_t)(4+i);
        ok=ok && !cellSpursAttachLv2EventQueue((void*)0x58000,queues[i],(uint8_t*)(uintptr_t)(0x50000+i),0);
    }
    auto receive=[&](uint32_t queue) {
        ppu_context ctx{};ctx.gpr[3]=queue;ctx.gpr[4]=0x51000;ctx.gpr[5]=16;ctx.gpr[6]=0x51200;
        if(sys_event_queue_tryreceive(&ctx)) return ~0u;
        return vm_read32(0x51200);
    };
    for(unsigned run=0;run<2;run++) {
        uint32_t binary=0x60000+run*0x100,desc=0x61000+run*0x100,chain=0x62000+run*0x100;
        memset(vm_base+binary,(int)(run+1),16);memset(vm_base+desc,0,256);
        spu_workload_register_img(spu_workload_fingerprint(vm_base+binary,16),event_test_job,202+run,"event-contract");
        vm_write64(desc,binary);vm_write32(desc+8,0x00010000);
        vm_write64(chain,desc);vm_write64(chain+8,127); // JOB then END
        ok=ok && !cellSpursCreateJobChain(0x58000,0x63000+run*0x100,chain,256,1,0,1,0);
        ok=ok && !cellSpursRunJobChain(0x63000+run*0x100);
        ok=ok && !cellSpursJoinJobChain(0x63000+run*0x100);
        uint32_t first=receive(queues[0]);
        bool payload=run==0 || (vm_read32(0x51010)==4 && vm_read32(0x51014)==7 && vm_read32(0x5101C)==0x12345678);
        uint32_t second=receive(queues[1]);
        bool pass=first==(run?1u:0u) && second==0 && payload && event_jobs==run+1;
        fprintf(stderr,"[compat-check] SPURS job %s: queueA=%u queueB=%u jobs=%u payload=%u %s\n",
            run?"real port4 event":"no event",first,second,event_jobs.load(),payload?1u:0u,pass?"PASS":"FAIL");
        ok=ok && pass;
        if(!pass) break;
    }
    return ok?0:1;
}

extern "C" int port_check_lfqueue()
{
    // Local game bytes supply constants for the actual image-42 queue code;
    // neither its implementation nor its outputs are replaced by a model.
    FILE* file=fopen("work/extracted/PS3_GAME/USRDIR/EBOOT.elf","rb");
    if(!file) return 2;
    std::vector<uint8_t> image(0x20000);
    fseek(file,0x1799780,SEEK_SET);
    size_t got=fread(image.data(),1,image.size(),file); fclose(file);
    size_t bytes=spu_elf_image_size(image.data(),got);
    auto spu=std::make_unique<spu_context>();
    spu_context_init(spu.get(),0); spu->image_id=42;
    uint32_t entry=0;
    if(!bytes || !spu_elf_load_to_ls(image.data(),bytes,spu->ls,&entry) ||
       spu_workload_fingerprint(image.data(),bytes)!=0x00E729804474449Aull) return 3;
    spu_begin_image(42); spu_0001_at_01799780_spu_recomp_register();
    constexpr uint32_t q=0x51000,buf=0x51100,input=0x52000,arg=0x38000,out=0x38100;
    memset(vm_base+q,0,128); memset(vm_base+buf,0xA5,512);
    port_register_queue_api();
    ppu_context ppu{}; ppu.gpr[1]=0x70000;
    ppu.gpr[3]=0x58001;ppu.gpr[4]=q;ppu.gpr[5]=buf;ppu.gpr[6]=32;ppu.gpr[7]=16;ppu.gpr[8]=3;
    ps3_hle_call(ps3_compute_nid("_cellSpursLFQueueInitialize"),&ppu);
    bool ok=!ppu.gpr[3];
    auto word=[&](uint32_t at,uint32_t v) { for(unsigned i=0;i<4;i++) spu->ls[at+i]=(uint8_t)(v>>(24-8*i)); };
    word(arg,out); word(arg+4,0); word(arg+8,0);
    bool blocking=false;
    auto invoke=[&](void(*fn)(spu_context*),bool pop) {
        spu->gpr[0]._u32[0]=0x3FFF0; spu->gpr[1]._u32[0]=0x3F000;
        memset(&spu->gpr[3],0,sizeof spu->gpr[3]); spu->gpr[3]._u32[1]=q;
        spu->gpr[4]._u32[0]=arg;spu->gpr[5]._u32[0]=pop?(blocking?1:0):1;
        fn(spu.get()); spu_drain_call(spu.get(),0x3FFF0);
        return spu->gpr[3]._u32[0];
    };
    // More than two full cursor cycles: real reserve, DMA copy and complete.
    for(unsigned i=0;i<65 && ok;i++) {
        for(unsigned k=0;k<32;k++) vm_base[input+k]=(uint8_t)(i+k);
        ppu.gpr[3]=q;ppu.gpr[4]=input;ppu.gpr[5]=3;
        ps3_hle_call(ps3_compute_nid("_cellSpursLFQueuePushBody"),&ppu);
        ok=!ppu.gpr[3] && invoke(spu_0001_at_01799780_spu_func_00009CB0,true)==0;
        ok=ok && !memcmp(spu->ls+out,vm_base+input,32);
        ok=ok && invoke(spu_0001_at_01799780_spu_func_00009AD8,false)==0;
        ok=ok && (vm_read32(q)>>16)==(i+1)%32 && (vm_read32(q+8)>>16)==(i+1)%32;
    }
    if(ok) {
        // Fill all sixteen slots, prove that a seventeenth PPU producer blocks,
        // then release it by completing a pop with the real guest SPU code.
        for(unsigned i=0;i<16;i++) {
            memset(vm_base+input,(int)(0x80+i),32);
            ppu.gpr[3]=q;ppu.gpr[4]=input;ppu.gpr[5]=3;
            ps3_hle_call(ps3_compute_nid("_cellSpursLFQueuePushBody"),&ppu);
            if(ppu.gpr[3]) ok=false;
        }
        std::atomic<bool> entered{false},done{false};
        uint32_t producer_rc=~0u;
        memset(vm_base+input,0xA0,32);
        std::thread producer([&] {
            ppu_context other{};other.gpr[1]=0x71000;
            other.gpr[3]=q;other.gpr[4]=input;other.gpr[5]=3;
            entered=true;
            ps3_hle_call(ps3_compute_nid("_cellSpursLFQueuePushBody"),&other);
            producer_rc=(uint32_t)other.gpr[3];
            done=true;
        });
        while(!entered) std::this_thread::yield();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        ok=ok && !done;
        ok=ok && invoke(spu_0001_at_01799780_spu_func_00009CB0,true)==0;
        ok=ok && spu->ls[out]==0x80 && invoke(spu_0001_at_01799780_spu_func_00009AD8,false)==0;
        auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
        while(!done && std::chrono::steady_clock::now()<deadline) std::this_thread::yield();
        if(!done) { fprintf(stderr,"[compat-check] LF full producer wake FAIL\n");std::exit(6); }
        producer.join();
        ok=ok && !producer_rc;
        for(unsigned i=1;i<=16 && ok;i++) {
            ok=invoke(spu_0001_at_01799780_spu_func_00009CB0,true)==0;
            uint8_t expected=i==16?0xA0:(uint8_t)(0x80+i);
            for(unsigned k=0;k<32;k++) if(spu->ls[out+k]!=expected) ok=false;
            ok=ok && invoke(spu_0001_at_01799780_spu_func_00009AD8,false)==0;
        }
        fprintf(stderr,"[compat-check] LF full blocking producer, actual SPU release and FIFO payloads: %s\n",ok?"PASS":"FAIL");
    }
    fprintf(stderr,"[compat-check] LF image42 sequential DMA/wrap: %s\n",ok?"PASS":"FAIL");
    constexpr uint32_t taskset=0x54000;
    uint32_t args[4]={q,0,0,0},pattern[4]={~0u,~0u,~0u,~0u};
    spurs_taskset_init(taskset,0x58000,0,0,0x1900,0,0);
    spurs_taskset_add_task(taskset,0,0x80000,0x60000,args,pattern);
    spurs_pm_build_context(spu->ls,taskset,0,0,0);
    word(0x2FB0,~0u);word(0x2FB4,0x400);word(0x2FBC,0x3000);
    port_lf_register_consumer(q,taskset,0);
    blocking=true;
    // The real guest registers its wait token and enters WAIT_SIGNAL. Exercise
    // all 30 ring positions, including publication before the host parks.
    for(unsigned i=0;i<31 && ok;i++) {
        std::atomic<bool> done{false}; uint32_t rc=~0u;
        std::thread consumer([&] { rc=invoke(spu_0001_at_01799780_spu_func_00009CB0,true); done=true; });
        auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
        bool waiting=false;
        while(!done && std::chrono::steady_clock::now()<deadline) {
            spu_lockline_lock(); uint32_t control=vm_read32(q+4)>>16; spu_lockline_unlock();
            if((control&31)!=((control>>10)&31)) { waiting=true; break; }
            std::this_thread::yield();
        }
        if(!waiting) { fprintf(stderr,"[compat-check] LF wait registration FAIL rc=%08X\n",rc); std::exit(4); }
        for(unsigned k=0;k<32;k++) vm_base[input+k]=(uint8_t)(i+97+k);
        ppu.gpr[3]=q;ppu.gpr[4]=input;ppu.gpr[5]=3;
        ps3_hle_call(ps3_compute_nid("_cellSpursLFQueuePushBody"),&ppu);
        while(!done && std::chrono::steady_clock::now()<deadline) std::this_thread::yield();
        if(!done) { fprintf(stderr,"[compat-check] LF wake FAIL at ring iteration %u\n",i); std::exit(5); }
        consumer.join();
        ok=!ppu.gpr[3] && !rc && !memcmp(spu->ls+out,vm_base+input,32);
        ok=ok && invoke(spu_0001_at_01799780_spu_func_00009AD8,false)==0;
    }
    spu_coh_unregister(spu.get());
    fprintf(stderr,"[compat-check] LF PPU publish -> image42 reserve/DMA/complete, 65 entries + 31 blocking wakes/ring wrap: %s\n",ok?"PASS":"FAIL");
    return ok?0:1;
}

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
