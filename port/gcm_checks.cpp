#include <cstdint>
#include <cstdio>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
extern "C" void port_gcm_contract_begin(uint32_t);
extern "C" uint32_t port_gcm_contract_get();
extern "C" void cellGcm_rsx_process_fifo();
extern "C" void vm_write32(uint64_t,uint32_t);
extern "C" uint32_t vm_read32(uint64_t);
extern "C" uint32_t ppu_hle_inject_base;
extern "C" void* cellGcm_fifo_kick_event();

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
