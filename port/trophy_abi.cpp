#include "ppu_recomp.h"
#include "ps3emu/nid.h"
#include <cstdint>
#include <cstring>
#include <initializer_list>

extern "C" unsigned char* vm_base;
extern "C" void vm_write32(uint64_t, uint32_t);
extern "C" void ps3_hle_register_ctx(uint32_t, const char*, void (*)(ppu_context*));
extern "C" int32_t sceNpTrophyGetGameInfo(int32_t, int32_t, void*, void*);
extern "C" void port_register_resource_api();
extern "C" void port_register_overlay_api();
extern "C" void port_register_queue_api();

static void game_info_be(ppu_context* ctx)
{
    uint32_t details = (uint32_t)ctx->gpr[5], data = (uint32_t)ctx->gpr[6];
    int32_t rc = sceNpTrophyGetGameInfo((int32_t)ctx->gpr[3], (int32_t)ctx->gpr[4],
        (void*)(uintptr_t)details, (void*)(uintptr_t)data);
    if (!rc) {
        // Backend writes native u32 fields through guest pointers. Strings
        // already have byte order; convert only the five scalar outputs.
        for (uint32_t address : {details, data}) {
            if (!address) continue;
            for (unsigned offset = 0; offset < 20; offset += 4) {
                uint32_t host_value;
                memcpy(&host_value, vm_base+address+offset, 4);
                vm_write32(address+offset, host_value);
            }
        }
        if (details) vm_write32(details+20+128+1024, 0); // ABI reserved[4]
    }
    ctx->gpr[3] = (uint64_t)(int64_t)rc;
}

extern "C" void port_register_compatibility()
{
    port_register_resource_api();
    port_register_overlay_api();
    port_register_queue_api();
    ps3_hle_register_ctx(ps3_compute_nid("sceNpTrophyGetGameInfo"),
                        "sceNpTrophyGetGameInfo (BE output)", game_info_be);
}
