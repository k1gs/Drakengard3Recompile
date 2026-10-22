#include "spu_context.h"
#include "spu_workload.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <memory>

extern "C" unsigned char* vm_base;
extern "C" void vm_write32(uint64_t,uint32_t);
extern "C" void port_taskset_lock();
extern "C" void port_taskset_unlock();
extern "C" int port_dispatch_task_locked(const uint8_t*,uint32_t,uint32_t,uint32_t,uint32_t);

static std::atomic<unsigned> completed{0}, failures{0};
static void task_entry(spu_context* ctx)
{
    uint32_t expected=ctx->image_id==201?0xA1110001u:0xB2220001u;
    uint32_t elf=ctx->image_id==201?0x80000u:0x81000u;
    uint32_t actual=((uint32_t)ctx->ls[0x2794]<<24)|((uint32_t)ctx->ls[0x2795]<<16)
                   |((uint32_t)ctx->ls[0x2796]<<8)|ctx->ls[0x2797];
    if(ctx->gpr[3]._u32[0]!=expected || actual!=elf
       || ctx->gpr[4]._u32[0]!=0x12345678u
       || ctx->gpr[4]._u32[1]!=expected) failures.fetch_add(1);
    completed.fetch_add(1);
}

extern "C" int port_check_spu_task_snapshot()
{
    auto image=[](uint32_t ea,uint8_t marker) {
        memset(vm_base+ea,0,0x90);
        const uint8_t ident[]={0x7f,'E','L','F',1,2,1};
        memcpy(vm_base+ea,ident,sizeof ident);
        auto half=[&](unsigned at,uint16_t value) { vm_base[ea+at]=(uint8_t)(value>>8); vm_base[ea+at+1]=(uint8_t)value; };
        half(16,2); half(18,23); vm_write32(ea+20,1); vm_write32(ea+24,0x3000); vm_write32(ea+28,52);
        half(40,52); half(42,32); half(44,1);
        vm_write32(ea+52,1); vm_write32(ea+56,0x80); vm_write32(ea+60,0x3000); vm_write32(ea+64,0x3000);
        vm_write32(ea+68,16); vm_write32(ea+72,16); vm_write32(ea+76,5); vm_write32(ea+80,16);
        vm_base[ea+0x80]=marker;
    };
    image(0x80000,1); image(0x81000,2);
    for(unsigned i=0;i<2;i++) {
        uint32_t ea=0x80000+i*0x1000,ts=0x90000+i*0x2000;
        memset(vm_base+ts,0,0x2000);
        vm_base[ts+0x10]=vm_base[ts+0x20]=vm_base[ts+0x30]=0x80;
        vm_write32(ts+0x64,0x98000);
        vm_write32(ts+0x68,0x12345678);
        vm_write32(ts+0x6C,i?0xB2220001u:0xA1110001u);
        vm_write32(ts+0x80,i?0xB2220001u:0xA1110001u); vm_write32(ts+0x94,ea);
        spu_workload_register_img(spu_workload_fingerprint(vm_base+ea,0x90),task_entry,201+i,"snapshot-check");
    }
    // Workers cannot reach their context builder until this lock is released.
    // Overwrite both guest slots after enqueue: each worker must retain its own args.
    port_taskset_lock();
    bool queued=port_dispatch_task_locked(vm_base+0x80000,0x90,0,0x90000,0)!=0;
    queued=queued && port_dispatch_task_locked(vm_base+0x80000,0x90,0,0x90000,0)!=0;
    queued=queued && port_dispatch_task_locked(vm_base+0x81000,0x90,0,0x92000,0)!=0;
    vm_write32(0x90080,0xCCCC0001); vm_write32(0x90094,0x81000);
    vm_write32(0x92080,0xDDDD0001); vm_write32(0x92094,0x80000);
    port_taskset_unlock();
    auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
    while(completed.load()<2 && std::chrono::steady_clock::now()<deadline) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    bool ok=queued && completed.load()==2 && failures.load()==0;
    fprintf(stderr,"[compat-check] task identity, queued argument snapshot, duplicate claim: %s (completed=%u failures=%u)\n",
        ok?"PASS":"FAIL",completed.load(),failures.load());
    return ok?0:1;
}

extern "C" int32_t _cellSpursTasksetAttributeInitialize(void*,uint32_t,uint32_t,uint64_t,uint64_t,uint32_t);
extern "C" int32_t cellSpursInitialize(void*,int32_t,int32_t,int32_t,uint8_t);
extern "C" int32_t cellSpursCreateTasksetWithAttribute(void*,void*,const void*);
extern "C" int32_t cellSpursFinalize(void*);
extern "C" uint64_t vm_read64(uint64_t);
extern "C" uint32_t vm_read32(uint64_t);
extern "C" uint64_t spurs_pm_build_context(uint8_t*,uint32_t,uint32_t,uint32_t,uint32_t);

extern "C" int port_check_taskset_arguments()
{
    constexpr uint32_t attr=0xA0000,priority=0xA0200,spurs=0xA1000,taskset=0xA4000;
    constexpr uint64_t args=0x1234567887654321ull;
    auto guest=[](uint32_t ea) { return reinterpret_cast<void*>(uintptr_t(ea)); };
    memset(vm_base+attr,0xCC,512);
    for(unsigned i=0;i<8;i++) vm_base[priority+i]=(uint8_t)(i+1);
    int32_t rc=_cellSpursTasksetAttributeInitialize(guest(attr),1,0x360001,args,priority,3);
    bool ok=!rc && vm_read64(attr+8)==args && !memcmp(vm_base+attr+16,vm_base+priority,8);
    ok=ok && vm_base[attr+511]==0 && vm_base[attr+512]==1;
    ok=ok && vm_read32(attr)==1 && vm_read32(attr+4)==0x360001
        && vm_read32(attr+24)==3 && vm_read32(attr+32)==6400;
    uint8_t saved[512];memcpy(saved,vm_base+attr,512);
    vm_base[priority]=16;
    rc=_cellSpursTasksetAttributeInitialize(guest(attr),1,0,0,priority,1);
    ok=ok && rc!=0 && !memcmp(saved,vm_base+attr,512);
    vm_base[priority]=1;
    rc=cellSpursInitialize(guest(spurs),1,100,100,0);
    ok=ok && !rc;
    rc=cellSpursCreateTasksetWithAttribute(guest(spurs),guest(taskset),guest(attr));
    ok=ok && !rc && vm_read64(taskset+0x68)==args;
    auto ctx=std::make_unique<spu_context>();spu_context_init(ctx.get(),0);
    spurs_pm_build_context(ctx->ls,taskset,0,0,0);
    const uint8_t expected[]={0x12,0x34,0x56,0x78,0x87,0x65,0x43,0x21};
    ok=ok && !memcmp(ctx->ls+0x2768,expected,8);
    cellSpursFinalize(guest(spurs));
    fprintf(stderr,"[compat-check] BE taskset attribute, creation and SPU context arguments: %s\n",ok?"PASS":"FAIL");
    return ok?0:1;
}
