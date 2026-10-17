// Windows demand paging for the flat guest address space.
#include "host.h"

extern "C"
{
    uint8_t *vm_base = nullptr;
    extern uint32_t ppu_vm_size;
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

    return vm_base != nullptr;
}
