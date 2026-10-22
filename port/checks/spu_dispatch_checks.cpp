#include "spu_context.h"
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <cstring>
#include <vector>

extern "C" void spu_indirect_branch(spu_context*);
extern "C" uint32_t spu_interp_run(spu_context*,uint32_t);
extern "C" int spu_run_with_halt(void(*)(spu_context*),spu_context*);
extern "C" void spu_indirect_branch_mt(spu_context*);
extern "C" void drakengard_spu_register_all();
extern "C" uint32_t spu_interp_run_until(spu_context*,uint32_t,uint32_t);
extern "C" void spu_0014_at_0182C580_spu_func_0000CAF0(spu_context*);
extern "C" void spu_0014_at_0182C580_spu_func_000032D0(spu_context*);
extern "C" void spu_0015_at_01841400_spu_func_00003260(spu_context*);
extern "C" void port_spu_set_test_clock(uint64_t(*)(void));
extern "C" unsigned char* vm_base;
static uint64_t replay_ns;
static uint64_t replay_clock() { replay_ns+=1000;return replay_ns; }

extern "C" int port_check_spu_task_replay()
{
    const char* input=std::getenv("PORT_TASK_REPLAY_INPUT");
    const char* dma=std::getenv("PORT_TASK_REPLAY_DMA");
    if(!input || !dma) return 2;
    auto initial=std::make_unique<spu_context>();
    spu_context_init(initial.get(),0);
    FILE* f=fopen(input,"rb");
    if(!f) return 2;
    bool loaded=fread(initial->gpr,sizeof initial->gpr,1,f)==1 && fread(initial->ls,SPU_LS_SIZE,1,f)==1;
    fclose(f);
    if(!loaded) return 2;
    f=fopen(dma,"rb");if(!f) return 2;
    uint32_t header[3];
    loaded=fread(header,sizeof header,1,f)==1 && header[0]==0x31414D44 && header[1]<=32 && header[2]==16384;
    struct Read { uint32_t ea;std::vector<uint8_t> bytes; };
    std::vector<Read> reads;
    for(unsigned i=0;loaded && i<header[1];i++) {
        uint32_t record[5];loaded=fread(record,sizeof record,1,f)==1;
        if(!loaded || record[2]>16384 || uint64_t(record[0])+record[2]>0x100000000ull) { loaded=false;break; }
        Read read{record[0],std::vector<uint8_t>(record[2])};
        loaded=fread(read.bytes.data(),1,record[2],f)==record[2];
        reads.push_back(std::move(read));
    }
    fclose(f);if(!loaded) return 2;
    auto seed=[&](){for(auto it=reads.rbegin();it!=reads.rend();++it) std::memcpy(vm_base+it->ea,it->bytes.data(),it->bytes.size());};
    const char* start_env=std::getenv("PORT_TASK_REPLAY_ENTRY");
    uint32_t start=start_env?(uint32_t)strtoul(start_env,nullptr,16):0xCAF0;
    if(start!=0xCAF0 && start!=0x32D0 && start!=0x3070) return 2;
    const bool producer=start==0x3070;
    const uint32_t stop=producer?0x3CB8:0xD138;
    initial->image_id=producer?56:55;initial->pc=start;initial->status=SPU_STATUS_RUNNING;
    if(producer) {
        seed();initial->image_id=200;
        spu_interp_run_until(initial.get(),start,0x3260);
        fprintf(stderr,"[task-replay] producer setup pc=%05X\n",initial->pc);
        if(initial->pc!=0x3260) return 1;
        initial->image_id=56;start=0x3260;
    }
    const char* inspect_stop=std::getenv("PORT_TASK_REPLAY_INSPECT");
    if(producer && inspect_stop) {
        seed();initial->image_id=200;
        const uint32_t at=(uint32_t)strtoul(inspect_stop,nullptr,16);
        spu_interp_run_until(initial.get(),start,at);
        FILE* out=fopen("task-replay-inspect.bin","wb");
        if(out) { fwrite(initial->gpr,sizeof initial->gpr,1,out);fwrite(initial->ls,1,SPU_LS_SIZE,out);fclose(out); }
        fprintf(stderr,"[task-replay] inspect pc=%05X\n",initial->pc);
        return initial->pc==at?0:1;
    }
    auto native=std::make_unique<spu_context>(*initial);
    auto interp=std::make_unique<spu_context>(*initial);
    native->ls=native->ls_store;
    interp->ls=interp->ls_store;
    interp->image_id=200; // No lifted entries: execute every instruction.
    if(start==0x32D0) {
        seed();
        spu_interp_run_until(interp.get(),start,0xCAF0);
        FILE* output=fopen("task-replay-caf0.bin","wb");
        if(output) { fwrite(interp->gpr,sizeof interp->gpr,1,output);fwrite(interp->ls,1,SPU_LS_SIZE,output);fclose(output); }
        fprintf(stderr,"[task-replay] interpreted setup pc=%05X root=%05X\n",interp->pc,interp->gpr[3]._u32[0]);
        return interp->pc==0xCAF0?0:1;
    }
    drakengard_spu_register_all();
    void(*fn)(spu_context*)=producer?spu_0015_at_01841400_spu_func_00003260:start==0xCAF0?spu_0014_at_0182C580_spu_func_0000CAF0:spu_0014_at_0182C580_spu_func_000032D0;
    seed();replay_ns=1000000;port_spu_set_test_clock(replay_clock);
    unsigned calls=0;
    while(fn && native->pc!=stop && calls++<100000) {
        g_spu_trampoline_fn=nullptr;fn(native.get());fn=g_spu_trampoline_fn;
    }
    g_spu_trampoline_fn=nullptr;
    seed();replay_ns=1000000;
    spu_interp_run_until(interp.get(),start,stop);
    port_spu_set_test_clock(nullptr);
    if(producer) {
        for(auto pair : {std::pair<const char*,spu_context*>{"task-replay-native.bin",native.get()}, {"task-replay-interp.bin",interp.get()}}) {
            FILE* out=fopen(pair.first,"wb");if(out) { fwrite(pair.second->gpr,sizeof pair.second->gpr,1,out);fwrite(pair.second->ls,1,SPU_LS_SIZE,out);fclose(out); }
        }
    }
    bool regs=!std::memcmp(native->gpr,interp->gpr,sizeof native->gpr);
    bool memory=!std::memcmp(native->ls,interp->ls,SPU_LS_SIZE);
    fprintf(stderr,"[task-replay] native-pc=%05X interp-pc=%05X registers=%d LS=%d root=%05X object=%05X\n",
        native->pc,interp->pc,regs,memory,native->gpr[88]._u32[0],native->gpr[81]._u32[0]);
    for(unsigned r=0;r<128;r++) if(std::memcmp(&native->gpr[r],&interp->gpr[r],16))
        fprintf(stderr,"[task-replay] r%u native=%08X/%08X/%08X/%08X interp=%08X/%08X/%08X/%08X\n",r,
          native->gpr[r]._u32[0],native->gpr[r]._u32[1],native->gpr[r]._u32[2],native->gpr[r]._u32[3],
          interp->gpr[r]._u32[0],interp->gpr[r]._u32[1],interp->gpr[r]._u32[2],interp->gpr[r]._u32[3]);
    return native->pc==stop && interp->pc==stop && regs && memory?0:1;
}

extern "C" int port_check_spu_task_null_branch()
{
    auto ctx=std::make_unique<spu_context>();
    spu_context_init(ctx.get(),0);
    // Image 55 is a task ELF whose executable section starts at LS 0x3000.
    // Zero at LS 0 is absent policy memory, not a task completion instruction.
    ctx->image_id=55;ctx->pc=0;
    spu_run_with_halt(spu_indirect_branch_mt,ctx.get());
    fprintf(stderr,"[compat-check] task null branch incorrectly completed\n");
    return 1;
}

extern "C" int port_check_spu_interpreter_halt()
{
    auto ctx=std::make_unique<spu_context>();
    spu_context_init(ctx.get(),0);
    ctx->image_id=200;
    // heqi r11,0: r11 starts at zero, so execution must stop with exit 8.
    constexpr uint32_t word=0x7F000580;
    for(unsigned i=0;i<4;i++) ctx->ls[0x37000+i]=(uint8_t)(word>>(24-i*8));
    spu_interp_run(ctx.get(),0x37000);
    fprintf(stderr,"[compat-check] interpreter HALT incorrectly returned\n");
    return 1;
}

extern "C" int port_check_spu_return()
{
    // A runtime-loaded callee returns to the middle of a lifted host function.
    // Continuing interpretation there executes the caller a second time.
    _putenv_s("SPU_INTERP_UNLIFTED", "1");
    auto ctx=std::make_unique<spu_context>();
    spu_context_init(ctx.get(),0);
    ctx->image_id=200;
    constexpr uint32_t entry=0x37000,ret=0x10004;
    auto word=[&](uint32_t at,uint32_t value) {
        ctx->ls[at]=(uint8_t)(value>>24); ctx->ls[at+1]=(uint8_t)(value>>16);
        ctx->ls[at+2]=(uint8_t)(value>>8); ctx->ls[at+3]=(uint8_t)value;
    };
    word(entry,0x40800383); // il r3,7
    word(entry+4,0x35000000); // bi r0
    word(ret,0x40803183); // il r3,99 -- caller continuation must not execute
    word(ret+4,0x00002000); // stop 0x2000
    ctx->gpr[0]._u32[0]=ret;
    ctx->pc=entry;
    spu_indirect_branch(ctx.get());
    bool ok=ctx->pc==ret && ctx->gpr[3]._u32[0]==7 && ctx->status==SPU_STATUS_RUNNING;
    fprintf(stderr,"[compat-check] unlifted callee returns before host continuation: %s (pc=%05X r3=%u status=%X)\n",
            ok?"PASS":"FAIL",ctx->pc,ctx->gpr[3]._u32[0],ctx->status);
    return ok?0:1;
}
