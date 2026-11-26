#pragma once
#include <stdint.h>

/* Guest-visible driver registers, offset tables and codec scratch. The RSX
 * back-end semaphore method uses a 24-bit offset into this address space.
 * Keep the window outside user memory, LOCAL, PPU stacks and raw SPU MMIO. */
#define PORT_SERVICE_BASE UINT32_C(0xF0000000)
#define PORT_SERVICE_SIZE UINT32_C(0x01000000)

static inline int port_invalid_user_mapping(uint64_t address, uint64_t size)
{
    const uint64_t limit = UINT64_C(1) << 32;
    if (address >= limit || size > limit - address)
        return 1;
    return size != 0 && address < PORT_SERVICE_BASE + PORT_SERVICE_SIZE &&
           address + size > PORT_SERVICE_BASE;
}
