// Windows demand paging for the flat guest address space.
#include "host.h"
#include "service_memory.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C"
{
    uint8_t *vm_base = nullptr;
    extern uint32_t ppu_vm_size;
    extern uint32_t ppu_hle_inject_base;
}

namespace
{
// Textures can overlap and different cache entries can describe the same bytes.
// Remember a write after Windows clears its bit, so every such entry sees it.
constexpr uint64_t address_limit = UINT64_C(1) << 32;
constexpr size_t region_size = 64 * 1024;
constexpr size_t query_pages = 1024;
uint64_t region_writes[address_limit / region_size];
uint64_t latest_write = 1;
DWORD watched_page_size;
}

// The D3D12 submission thread is the only reader/resetter of these write bits.
// PPU, SPU and RSX writers continue to write directly to the flat mapping.
// Zero requests an exact byte comparison instead.
extern "C" uint64_t port_texture_source_version(uint32_t ea, size_t bytes)
{
    if (!watched_page_size || bytes < region_size || bytes > address_limit - ea)
        return 0;

    const uint64_t end = uint64_t(ea) + bytes;
    const uint64_t page_mask = uint64_t(watched_page_size) - 1;
    const uint64_t page_end = (end + page_mask) & ~page_mask;
    static void *written[query_pages];
    for (uint64_t page = ea & ~page_mask; page < page_end;)
    {
        const size_t length = static_cast<size_t>(
            std::min<uint64_t>(page_end - page, query_pages * watched_page_size));
        ULONG_PTR count = query_pages;
        DWORD granularity = 0;
        // Read and reset atomically, with room for every page in this range.
        if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, vm_base + page, length, written, &count,
                          &granularity) != 0 || granularity != watched_page_size)
        {
            watched_page_size = 0;
            std::fprintf(stderr, "[texture-cache] write tracking unavailable; comparing source bytes\n");
            return 0;
        }
        if (count)
        {
            if (++latest_write == 0)
            {
                watched_page_size = 0;
                return 0;
            }
            for (ULONG_PTR i = 0; i < count; ++i)
            {
                const auto offset = static_cast<uint8_t *>(written[i]) - vm_base;
                region_writes[offset / region_size] = latest_write;
            }
        }
        page += length;
    }
    uint64_t version = 1;
    for (size_t i = ea / region_size; i <= (end - 1) / region_size; ++i)
        version = (std::max)(version, region_writes[i]);
    return version;
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
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    const char *setting = std::getenv("PORT_TEXTURE_WRITE_WATCH");
    if ((!setting || std::strcmp(setting, "0") != 0) && info.dwPageSize &&
        region_size % info.dwPageSize == 0)
    {
        vm_base = static_cast<uint8_t *>(VirtualAlloc(nullptr, kGuestVmSize,
            MEM_RESERVE | MEM_WRITE_WATCH, PAGE_READWRITE));
        if (vm_base)
            watched_page_size = info.dwPageSize;
    }
    if (!vm_base)
        vm_base = static_cast<uint8_t *>(VirtualAlloc(nullptr, kGuestVmSize, MEM_RESERVE, PAGE_READWRITE));
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
