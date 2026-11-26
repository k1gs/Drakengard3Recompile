#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <cstring>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "ppu_recomp.h"
extern "C" PPU_THREAD_LOCAL ppu_context* g_active_ctx;
extern "C" void port_gcm_contract_begin(uint32_t);
extern "C" uint32_t port_gcm_contract_get();
extern "C" void port_gcm_contract_default_context(uint32_t);
extern "C" int port_check_fifo_callback_recycle(uint32_t);
extern "C" void cellGcm_rsx_process_fifo();
extern "C" void vm_write32(uint64_t,uint32_t);
extern "C" uint32_t vm_read32(uint64_t);
extern "C" uint32_t ppu_hle_inject_base;
extern "C" void* cellGcm_fifo_kick_event();
extern "C" int32_t cellGcmSetDisplayBuffer(uint32_t,uint32_t,uint32_t,uint32_t,uint32_t);
extern "C" int32_t cellGcmSetPrepareFlip(void*,uint32_t);
extern "C" int32_t cellGcmSetFlipImmediate(uint32_t);
extern "C" uint32_t cellGcmGetCurrentDisplayBufferId();
extern "C" int32_t cellGcmGetDisplayBufferByFlipIndex(uint32_t);
extern "C" unsigned cellGcm_flip_request_count();
extern "C" void cellGcmSetFlipHandler(void*);
extern "C" void cellGcmSetUserHandler(void*);
extern "C" void cellGcmTickFlip();
extern "C" void ppu_gcm_pump();
using GuestCaller=void(*)(uint32_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t);
extern "C" GuestCaller g_ps3_guest_caller;
static unsigned flip_callbacks;
static void count_flip(uint32_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t)
{
    ++flip_callbacks;
}

extern "C" void port_gcm_contract_render_state(uint32_t[8]);

static unsigned user_callback_count;
static uint32_t user_callback_cause;

static void acknowledge_user_command(uint32_t, uint64_t cause, uint64_t, uint64_t,
                                     uint64_t, uint64_t, uint64_t, uint64_t, uint64_t)
{
    ++user_callback_count;
    user_callback_cause = static_cast<uint32_t>(cause);
    vm_write32(ppu_hle_inject_base + 0xFE0, user_callback_cause & 0xFFFF);
}

static bool check_user_command_wait()
{
    constexpr uint32_t fifo = 0x60000000;
    const uint32_t control = ppu_hle_inject_base + 0x2000;
    const uint32_t label = ppu_hle_inject_base + 0xFE0;
    const auto previous_caller = g_ps3_guest_caller;
    ppu_gcm_pump();
    g_ps3_guest_caller = acknowledge_user_command;
    cellGcmSetUserHandler(reinterpret_cast<void*>(static_cast<uintptr_t>(fifo + 0x100)));
    bool passed = true;
    for (uint32_t cause : {0u, 0x00010001u}) {
        port_gcm_contract_begin(fifo);
        vm_write32(label, 0xFF);
        vm_write32(fifo, 0x0004EB00);
        vm_write32(fifo + 4, cause);
        vm_write32(fifo + 8, 0x000C0064);
        vm_write32(fifo + 12, 0xFE0);
        vm_write32(fifo + 16, cause & 0xFFFF);
        vm_write32(fifo + 20, 0xFF);
        vm_write32(control, 24);
        user_callback_count = 0;
        cellGcm_rsx_process_fifo();
        const bool waiting = port_gcm_contract_get() == 8 && !user_callback_count && vm_read32(label) == 0xFF;
        // The guest can wait for REF without crossing another HLE import.
        // A host-side register inspection must not run guest callbacks.
        auto* previous_context = g_active_ctx;
        g_active_ctx = nullptr;
        vm_read32(control + 8);
        const bool host_read_passive = user_callback_count == 0;
        ppu_context polling_guest{};
        g_active_ctx = &polling_guest;
        vm_read32(control + 8);
        g_active_ctx = previous_context;
        const bool delivered = user_callback_count == 1 && user_callback_cause == cause &&
                               vm_read32(label) == (cause & 0xFFFF);
        cellGcm_rsx_process_fifo();
        ppu_gcm_pump();
        const bool finished = port_gcm_contract_get() == 24 && vm_read32(label) == 0xFF &&
                              user_callback_count == 1;
        passed = passed && waiting && host_read_passive && delivered && finished;
    }
    cellGcmSetUserHandler(nullptr);
    g_ps3_guest_caller = previous_caller;
    return passed;
}

extern "C" int port_check_fifo_semaphore()
{
    constexpr uint32_t fifo = 0x60000000;
    constexpr uint32_t offset = 0xFE0;
    const uint32_t label = ppu_hle_inject_base + offset;
    const uint32_t control = ppu_hle_inject_base + 0x2000;
    port_gcm_contract_begin(fifo);

    // Two ACQUIRE arguments in one non-incrementing packet. Once the first
    // value has been observed, the producer may replace it with the second.
    vm_write32(fifo, 0x00040064);
    vm_write32(fifo + 4, offset);
    vm_write32(fifo + 8, 0x40080068);
    vm_write32(fifo + 12, 0x1234);
    vm_write32(fifo + 16, 0x5678);
    vm_write32(fifo + 20, 0x0004006C);
    vm_write32(fifo + 24, 0xCAFE);
    vm_write32(control, 28);
    vm_write32(label, 0);
    cellGcm_rsx_process_fifo();
    const bool held_first = port_gcm_contract_get() == 8 && vm_read32(label) == 0;
    vm_write32(label, 0x1234);
    cellGcm_rsx_process_fifo();
    cellGcm_rsx_process_fifo();
    const bool held_second = port_gcm_contract_get() == 8 && vm_read32(label) == 0x1234;
    vm_write32(label, 0x5678);
    cellGcm_rsx_process_fifo();
    const bool released = port_gcm_contract_get() == 28 && vm_read32(label) == 0xCAFE;

    // OFFSET, ACQUIRE and RELEASE may also share an incrementing packet.
    vm_write32(fifo + 28, 0x000C0064);
    vm_write32(fifo + 32, offset);
    vm_write32(fifo + 36, 0xABCD);
    vm_write32(fifo + 40, 0xFACE);
    vm_write32(control, 44);
    cellGcm_rsx_process_fifo();
    const bool held_packet = port_gcm_contract_get() == 28 && vm_read32(label) == 0xCAFE;
    vm_write32(label, 0xABCD);
    cellGcm_rsx_process_fifo();
    const bool finished = port_gcm_contract_get() == 44 && vm_read32(label) == 0xFACE;

    const bool user_command = check_user_command_wait();
    const bool passed = held_first && held_second && released && held_packet && finished && user_command;
    std::fprintf(stderr, "[compat-check] FIFO semaphore held-first=%d held-second=%d "
                 "released=%d held-packet=%d finished=%d user-command=%d %s\n",
                 held_first, held_second, released, held_packet, finished, user_command, passed ? "PASS" : "FAIL");
    return passed ? 0 : 1;
}

extern "C" int port_check_fifo_engine_isolation()
{
    constexpr uint32_t ea=0x60000000;
    port_gcm_contract_begin(ea);
    const uint32_t control=ppu_hle_inject_base+0x2000;
    uint32_t pos=0;
    auto method=[&](uint32_t subchannel,uint32_t reg,uint32_t value) {
        vm_write32(ea+pos,0x40000u|(subchannel<<13)|reg);
        vm_write32(ea+pos+4,value);pos+=8;
    };
    method(0,0x30C,0x55);method(0,0x310,1);method(0,0x314,0x10001);
    method(0,0x318,0);method(0,0x31C,0x12345678);method(0,0x320,0x80068006);
    method(0,0x324,0);method(0,0x328,0);
    vm_write32(control,pos);cellGcm_rsx_process_fifo();
    uint32_t before[8],after[8];port_gcm_contract_render_state(before);
    // Captured NV0039 transfer fields overlap NV4097 alpha/blend/mask/stencil.
    // Exercise an incrementing FIFO packet, not a second copy of the router.
    method(1,0x184,0x66626660);method(1,0x188,0xFEED0001);
    vm_write32(ea+pos,(8u<<18)|(1u<<13)|0x30Cu);pos+=4;
    for(uint32_t value : {0u,0x300FB0u,0u,0u,0x410u,1u,0x101u,0u}) {
        vm_write32(ea+pos,value);pos+=4;
    }
    method(1,0x310,0);
    vm_write32(control,pos);cellGcm_rsx_process_fifo();
    port_gcm_contract_render_state(after);
    bool isolated=!memcmp(before,after,sizeof(before));
    bool consumed=port_gcm_contract_get()==pos;
    // Real 3D commands at those same offsets must still reach NV4097.
    method(0,0x324,0x01010101);method(0,0x310,0);
    vm_write32(control,pos);cellGcm_rsx_process_fifo();
    port_gcm_contract_render_state(after);
    bool render=after[0]==0x01010101 && after[1]==0 && port_gcm_contract_get()==pos;
    bool ok=isolated&&consumed&&render;
    fprintf(stderr,"[compat-check] FIFO engines: copy-keeps-render-state=%d consumed=%d 3D-updates=%d %s\n",
        isolated,consumed,render,ok?"PASS":"FAIL");
    return ok?0:1;
}

static bool check_display_queue(bool active_default)
{
    constexpr uint32_t fifo = 0x60000000;
    constexpr uint32_t context = fifo + 0x80000;
    const uint32_t control = ppu_hle_inject_base + 0x2000;
    port_gcm_contract_begin(fifo);
    for (uint32_t id : {2u, 0u, 1u}) {
        if (cellGcmSetDisplayBuffer(id, id * 0x400000u, 5120, 1280, 720))
            return false;
    }

    vm_write32(context, fifo);
    vm_write32(context + 4, fifo + 0x10000);
    vm_write32(context + 8, fifo);
    vm_write32(context + 12, 0);
    if (active_default)
        port_gcm_contract_default_context(context);

    ppu_gcm_pump();
    auto saved_caller = g_ps3_guest_caller;
    g_ps3_guest_caller = count_flip;
    flip_callbacks = 0;
    vm_write32(context + 0x20, 0x1234);
    cellGcmSetFlipHandler((void*)(uintptr_t)(context + 0x20));

    const auto command_context = (void*)(uintptr_t)context;
    unsigned requests = cellGcm_flip_request_count();
    uint32_t cursor = 0;
    int32_t slots[7] = {};
    bool packets = true, indices = true, capacity = true, presentation = true;
    // Seven usable entries in an eight-entry ring. Two batches cross index 0.
    for (unsigned batch = 0; batch < 2; ++batch) {
        for (unsigned i = 0; i < 7; ++i) {
            const uint32_t id = (i + 2) % 3;
            slots[i] = cellGcmSetPrepareFlip(command_context, id);
            const bool valid = slots[i] >= 0 && slots[i] < 8;
            indices &= valid && cellGcmGetDisplayBufferByFlipIndex(slots[i]) == (int32_t)id;
            for (unsigned j = 0; j < i; ++j)
                indices &= slots[i] != slots[j];
            packets &= vm_read32(fifo + cursor) == 0x0004E944 &&
                       vm_read32(fifo + cursor + 4) == id;
            cursor += 8;
            packets &= vm_read32(context + 8) == fifo + cursor;
        }
        capacity &= cellGcmSetPrepareFlip(command_context, 1) < 0 &&
                    vm_read32(context + 8) == fifo + cursor;
        presentation &= cellGcmSetFlipImmediate(slots[0]) < 0;
        packets &= vm_read32(control) == (active_default ? cursor : cursor - 56);
        vm_write32(control, cursor);
        cellGcm_rsx_process_fifo();
        ppu_gcm_pump();
        presentation &= port_gcm_contract_get() == cursor &&
                        cellGcm_flip_request_count() == requests;
        // Select a later queued buffer. The driver retires the preceding entries.
        for (unsigned i : {2u, 6u}) {
            unsigned callbacks = flip_callbacks;
            presentation &= cellGcmSetFlipImmediate(slots[i]) == 0;
            presentation &= cellGcmGetCurrentDisplayBufferId() == (i + 2) % 3;
            presentation &= cellGcm_flip_request_count() == ++requests;
            ppu_gcm_pump();
            presentation &= flip_callbacks == callbacks;
            cellGcmTickFlip();
            ppu_gcm_pump();
            ppu_gcm_pump();
            presentation &= flip_callbacks == callbacks + 1;
            for (unsigned old = 0; old <= i; ++old) {
                indices &= cellGcmGetDisplayBufferByFlipIndex(slots[old]) == -1;
                presentation &= cellGcmSetFlipImmediate(slots[old]) < 0;
            }
        }
    }

    cellGcmSetFlipHandler(nullptr);
    g_ps3_guest_caller = saved_caller;
    const bool ok = packets && indices && capacity && presentation;
    fprintf(stderr, "[compat-check] display queue default=%d packets=%d indices=%d "
            "capacity=%d presentation=%d callbacks=%u %s\n", active_default, packets,
            indices, capacity, presentation, flip_callbacks, ok ? "PASS" : "FAIL");
    return ok;
}

extern "C" int port_check_prepare_flip()
{
    const bool custom = check_display_queue(false);
    const bool active = check_display_queue(true);
    return custom && active ? 0 : 1;
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
    // A guest flip can leave a fence in the old ring tail. Neither that tail
    // nor commands beyond the published PUT may be discarded by host recycle.
    {
        constexpr uint32_t ea=0x60000000,context=ea+0x80000,end=ea+0x40000;
        uint32_t control=ppu_hle_inject_base+0x2000,label=ppu_hle_inject_base+0xFF0;
        port_gcm_contract_begin(ea);
        port_gcm_contract_default_context(context);
        vm_write32(context,ea);vm_write32(context+4,end);vm_write32(context+8,end);
        // Exercise the existing safely drained overrun path to enable its
        // near-end policy, just as a long-running title does before the stall.
        cellGcm_rsx_process_fifo();
        extern unsigned char* vm_base;
        memset(vm_base+ea,0,0x40000);
        vm_write32(context+8,end-8);
        vm_write32(label,0);
        constexpr uint32_t flip=0x20000;
        vm_write32(ea+flip,0x0004E924);vm_write32(ea+flip+4,0);
        vm_write32(ea+flip+8,0x00041D6C);vm_write32(ea+flip+12,0xFF0);
        vm_write32(ea+flip+16,0x00041D70);vm_write32(ea+flip+20,0x00660400);
        vm_write32(control,flip+24);
        cellGcm_rsx_process_fifo();
        bool tail=port_gcm_contract_get()==flip+8 && vm_read32(control)==flip+24 && vm_read32(label)==0;
        cellGcm_rsx_process_fifo();
        bool unpublished=port_gcm_contract_get()==flip+24 && vm_read32(context+8)==end-8 && vm_read32(label)==0x466;
        // Once the producer publishes those remaining NOPs and the owner reads
        // them, recycling is safe and still has to work.
        vm_write32(control,0x40000-8);
        cellGcm_rsx_process_fifo();
        bool recycled=port_gcm_contract_get()==0 && vm_read32(context+8)==ea && vm_read32(label)==0x466;
        fprintf(stderr,"[compat-check] FIFO recycle: flip-tail-held=%d unpublished-held=%d drained-recycled=%d %s\n",
            tail,unpublished,recycled,tail&&unpublished&&recycled?"PASS":"FAIL");
        ok=ok&&tail&&unpublished&&recycled;
    }
    constexpr uint32_t callback_ea=0x60000000;
    port_gcm_contract_begin(callback_ea);
    return port_check_fifo_callback_recycle(callback_ea) && ok?0:1;
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
