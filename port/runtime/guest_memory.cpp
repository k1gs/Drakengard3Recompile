// Windows demand paging for the flat guest address space.
#include "host.h"
#include "service_memory.h"

extern "C"
{
    uint8_t *vm_base = nullptr;
    extern uint32_t ppu_vm_size;
    extern uint32_t ppu_hle_inject_base;
}

static LONG WINAPI vm_commit_veh(EXCEPTION_POINTERS *ep)
{
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION)
    {
        ULONG_PTR fault = ep->ExceptionRecord->ExceptionInformation[1];
        uintptr_t base = (uintptr_t)vm_base;
        if (vm_base && fault >= base && fault < base + kGuestVmSize)
        {
            void *page = (void *)(fault & ~(uintptr_t)0xFFFF);
            if (VirtualAlloc(page, 0x10000, MEM_COMMIT, PAGE_READWRITE))
                return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

bool port_allocate_guest_vm(void)
{
    AddVectoredExceptionHandler(1, vm_commit_veh);
    vm_base = (uint8_t *)VirtualAlloc(NULL, kGuestVmSize, MEM_RESERVE, PAGE_READWRITE);
    ppu_vm_size = 0; /* the whole space is backed; no OOB guard needed */

    if (!vm_base)
        return false;

    // Drivers can be the first writers, including DMA from a worker thread.
    if (!VirtualAlloc(vm_base + PORT_SERVICE_BASE, PORT_SERVICE_SIZE, MEM_COMMIT, PAGE_READWRITE))
    {
        VirtualFree(vm_base, 0, MEM_RELEASE);
        vm_base = nullptr;
        return false;
    }
    ppu_hle_inject_base = PORT_SERVICE_BASE;
    return true;
}
