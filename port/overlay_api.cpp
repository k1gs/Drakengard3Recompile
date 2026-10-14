#include "ppu_recomp.h"
#include "ps3emu/nid.h"
#include <cstdio>
#include <cstdint>
#include "spu_elf_view.h"

extern "C" uint8_t* vm_base;
extern "C" void ps3_hle_register_ctx(uint32_t, const char*, void (*)(ppu_context*));
extern "C" [[noreturn]] void port_compatibility_blocker(const char*);

static port::SpuImageKind image_kind(uint64_t address)
{
    if(!address || address>0xFFC00000ull) return port::SpuImageKind::malformed;
    return port::inspect_spu_image(vm_base+(uint32_t)address,0x400000);
}

static void overlay_size(ppu_context* ctx)
{
    // One ELF argument, scalar size result; no invented cellOvisInit requirement.
    uint64_t image=ctx->gpr[3];
    auto kind=image_kind(image);
    if(kind==port::SpuImageKind::malformed) {
        // Actual firmware error, explicitly handled by the title at AD27F0..F8.
        // Empty embedded program markers must not become success or a byte size.
        fprintf(stderr,"[ovis] invalid/empty ELF %08llX -> CELL_OVIS_ERROR_INVAL\n",(unsigned long long)image);
        ctx->gpr[3]=(uint64_t)(int64_t)(int32_t)0x80410402u;
        return;
    }
    if(kind==port::SpuImageKind::overlays) port_compatibility_blocker("cellOvisGetOverlayTableSize: real overlays unsupported");
    fprintf(stderr,"[ovis] resident-only ELF %08llX: no overlapping LOAD ranges or overlay section; table size=0\n",(unsigned long long)image);
    ctx->gpr[3]=0;
}

static void overlay_init(ppu_context* ctx)
{
    auto kind=image_kind(ctx->gpr[4]);
    if(kind==port::SpuImageKind::malformed) { ctx->gpr[3]=(uint64_t)(int64_t)(int32_t)0x80410402u; return; }
    if(kind==port::SpuImageKind::overlays) port_compatibility_blocker("cellOvisInitializeOverlayTable: real overlays unsupported");
    fprintf(stderr,"[ovis] InitializeOverlayTable: empty resident-only table\n");
    ctx->gpr[3]=0;
}

extern "C" void port_register_overlay_api()
{
    ps3_hle_register_ctx(ps3_compute_nid("cellOvisGetOverlayTableSize"),
                        "cellOvisGetOverlayTableSize", overlay_size);
    ps3_hle_register_ctx(ps3_compute_nid("cellOvisInitializeOverlayTable"),
                        "cellOvisInitializeOverlayTable", overlay_init);
}
