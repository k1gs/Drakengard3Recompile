#include "spu_context.h"

typedef void (*port_spu_fn)(spu_context *);
extern port_spu_fn spu_lookup(uint32_t, int);
extern void spu_begin_image(int);
extern void port_audio_decoder_register(void);
extern port_spu_fn port_audio_decoder_lookup(const spu_context *, uint32_t);

#ifdef PORT_WITH_MSDSP
#include "msdsp_bytes.h"
enum
{
    DSP_IMAGE = 181,
    DSP_BASE = 0x37000
};
static int native_enabled; /* Set before starting any guest threads. */

static int matches(const spu_context *ctx)
{
    /* Validate every module byte, not just the first plugin's GUID. Heap
     * addresses vary; the compiled PIC code is valid only at these LS bases.
     * Padding between modules is not executable and need not match. */
    return ctx && ctx->ls && ctx->image_id == 41 &&
           !memcmp(ctx->ls + 0x37000, port_msdsp_bytes, 0x1F80) &&
           !memcmp(ctx->ls + 0x39000, port_msdsp_bytes + 0x2000, 0x890) &&
           !memcmp(ctx->ls + 0x39900, port_msdsp_bytes + 0x2900, 0x420);
}
#endif

void port_msdsp_register(void)
{
    port_audio_decoder_register();
#ifdef PORT_WITH_MSDSP
    const char *e = getenv("PORT_MSDSP_NATIVE");
    native_enabled = !(e && e[0] == '0');
    spu_begin_image(DSP_IMAGE);
    port_msdsp_spu_recomp_register();
#endif
}

port_spu_fn port_msdsp_lookup(const spu_context *ctx, uint32_t pc)
{
    if (pc == 0x21D20u && ctx && ctx->image_id == 41) {
        port_spu_fn audio = port_audio_decoder_lookup(ctx, pc);
        if (audio) return audio;
    }
#ifdef PORT_WITH_MSDSP
    if (!ctx || ctx->image_id != 41 || pc < DSP_BASE || pc >= 0x39D20)
        return NULL;
    if (!native_enabled)
        return NULL;
    port_spu_fn fn = spu_lookup(pc, DSP_IMAGE);
    if (!fn || !matches(ctx))
        return NULL;
    static _Thread_local int announced;
    if (!announced)
    {
        announced = 1;
        fprintf(stderr, "[msdsp-native] full module match; compiled DSP entry=%05X image=%d\n", pc,
                ctx->image_id);
    }
    return fn;
#else
    (void)ctx;
    (void)pc;
    return NULL;
#endif
}
