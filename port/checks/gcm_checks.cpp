#include <cstdint>
#include <cstdio>
#include <initializer_list>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
extern "C" void port_gcm_contract_begin(uint32_t);
extern "C" uint32_t port_gcm_contract_get();
extern "C" void port_gcm_contract_default_context(uint32_t);
extern "C" void cellGcm_rsx_process_fifo();
extern "C" void vm_write32(uint64_t,uint32_t);
extern "C" uint32_t vm_read32(uint64_t);
extern "C" uint32_t ppu_hle_inject_base;
extern "C" void* cellGcm_fifo_kick_event();
extern "C" int32_t cellGcmSetDisplayBuffer(uint32_t,uint32_t,uint32_t,uint32_t,uint32_t);
extern "C" int32_t cellGcmSetPrepareFlip(void*,uint32_t);
extern "C" unsigned cellGcm_flip_request_count();
extern "C" void cellGcmSetFlipHandler(void*);
extern "C" void cellGcmTickFlip();
extern "C" void ppu_gcm_pump();
using GuestCaller=void(*)(uint32_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t);
extern "C" GuestCaller g_ps3_guest_caller;
static unsigned flip_callbacks;
static void count_flip(uint32_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t)
{
    ++flip_callbacks;
}

extern "C" int port_check_prepare_flip()
{
    bool ok=true;
    for(bool active_default : {false,true}) {
    constexpr uint32_t ea=0x60000000,context=ea+0x80000;
    port_gcm_contract_begin(ea);
    if(cellGcmSetDisplayBuffer(2,0,5120,1280,720)) return 2;
    uint32_t control=ppu_hle_inject_base+0x2000;
    vm_write32(context,ea);vm_write32(context+4,ea+0x10000);
    vm_write32(context+8,ea);vm_write32(context+12,0);
    vm_write32(ea,0);vm_write32(ea+4,0);
    auto saved_caller=g_ps3_guest_caller;
    ppu_gcm_pump();
    flip_callbacks=0;
    g_ps3_guest_caller=count_flip;
    vm_write32(context+0x20,0x1234);
    cellGcmSetFlipHandler((void*)(uintptr_t)(context+0x20));
    if(active_default) port_gcm_contract_default_context(context);
    unsigned before=cellGcm_flip_request_count();
    int32_t result=cellGcmSetPrepareFlip((void*)(uintptr_t)context,2);
    bool deferred=result==2 && cellGcm_flip_request_count()==before && port_gcm_contract_get()==0;
    bool queued=vm_read32(ea)==0x0004E924 && vm_read32(ea+4)==2 && vm_read32(context+8)==ea+8;
    bool published=vm_read32(control)==(active_default?8u:0u);
    vm_write32(control,8);
    cellGcm_rsx_process_fifo();
    bool executed=port_gcm_contract_get()==8 && cellGcm_flip_request_count()==before+1;
    ppu_gcm_pump();
    bool no_early_callback=flip_callbacks==0;
    cellGcmTickFlip();
    bool deferred_completion=flip_callbacks==0;
    ppu_gcm_pump();
    bool completed_once=flip_callbacks==1;
    ppu_gcm_pump();
    completed_once=completed_once&&flip_callbacks==1;
    cellGcmSetFlipHandler(nullptr);
    g_ps3_guest_caller=saved_caller;
    bool pass=deferred&&queued&&published&&executed&&no_early_callback&&deferred_completion&&completed_once;
    fprintf(stderr,"[compat-check] prepare flip default=%d deferred=%d queued=%d published=%d consumed-once=%d %s\n",
        active_default,deferred,queued,published,executed,pass?"PASS":"FAIL");
    fprintf(stderr,"[compat-check] flip callback FIFO-quiet=%d completion-deferred=%d delivered-once=%d\n",
        no_early_callback,deferred_completion,completed_once);
    ok=ok&&pass;
    }
    return ok?0:1;
}

extern "C" int port_check_fifo_flip_boundary()
{
    bool ok=true;
    for(uint32_t method : {0xE920u,0xE924u}) {
        constexpr uint32_t ea=0x60000000;
        port_gcm_contract_begin(ea);
        if(cellGcmSetDisplayBuffer(0,0,5120,1280,720)) return 2;
        uint32_t control=ppu_hle_inject_base+0x2000,label=ppu_hle_inject_base+0xFF0;
        vm_write32(label,0);
        vm_write32(ea,0x00040000|method);vm_write32(ea+4,0);
        vm_write32(ea+8,0x00041D6C);vm_write32(ea+12,0xFF0);
        vm_write32(ea+16,0x00041D70);vm_write32(ea+20,0x00660400);
        vm_write32(control,24);
        cellGcm_rsx_process_fifo();
        bool stopped=port_gcm_contract_get()==8 && vm_read32(label)==0;
        cellGcm_rsx_process_fifo();
        bool resumed=port_gcm_contract_get()==24 && vm_read32(label)==0x466;
        fprintf(stderr,"[compat-check] FIFO flip %04X: next-frame-held=%d resumed=%d %s\n",
            method,stopped,resumed,stopped&&resumed?"PASS":"FAIL");
        ok=ok&&stopped&&resumed;
    }
    return ok?0:1;
}

extern "C" int port_check_gcm_park()
{
    constexpr uint32_t ea=0x60000000;
    port_gcm_contract_begin(ea);
    uint32_t control=ppu_hle_inject_base+0x2000,label=ppu_hle_inject_base+0xFF0;
    HANDLE wake=(HANDLE)cellGcm_fifo_kick_event();
    ResetEvent(wake);
    vm_write32(label,0);
    vm_write32(ea,0x20000000); // JUMP to self: producer has not released this segment.
    vm_write32(ea+4,0x00041D6C); vm_write32(ea+8,0xFF0);
    vm_write32(ea+12,0x00041D70); vm_write32(ea+16,0x00660400); // wire value -> 0x466
    vm_write32(control,20);
    bool put_woke=WaitForSingleObject(wake,0)==WAIT_OBJECT_0;
    // Repeated parked passes must not let a watchdog discard the pending packet.
    for(unsigned i=0;i<12;i++) cellGcm_rsx_process_fifo();
    bool held=port_gcm_contract_get()==0 && vm_read32(label)==0;
    ResetEvent(wake);
    vm_write32(ea+32,0);
    bool unrelated_quiet=WaitForSingleObject(wake,0)==WAIT_TIMEOUT;
    vm_write32(ea,0); // Actual producer patch releases the stream.
    bool patch_woke=WaitForSingleObject(wake,0)==WAIT_OBJECT_0;
    bool no_completion=port_gcm_contract_get()==0 && vm_read32(label)==0;
    cellGcm_rsx_process_fifo();
    bool resumed=port_gcm_contract_get()==20 && vm_read32(label)==0x466;
    fprintf(stderr,"[compat-check] FIFO self-park retained=%s producer-patch resumes=%s label=%08X: %s\n",
        held?"yes":"no",resumed?"yes":"no",vm_read32(label),held&&resumed?"PASS":"FAIL");
    bool wakes=put_woke && unrelated_quiet && patch_woke && no_completion;
    fprintf(stderr,"[compat-check] FIFO wake PUT=%d unrelated-quiet=%d patch=%d no-synthetic-completion=%d %s\n",
        put_woke,unrelated_quiet,patch_woke,no_completion,wakes?"PASS":"FAIL");
    return held&&resumed&&wakes?0:1;
}
