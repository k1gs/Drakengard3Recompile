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

static const struct dsp_module
{
    uint32_t base;
    uint32_t size;
} modules[] = {{DSP_BASE, 0x1F80}, {0x39000, 0x890}, {0x39900, 0x420}};

static int matches(const spu_context *ctx, uint32_t pc)
{
    int contains_pc = 0;
    for (size_t i = 0; i < sizeof(modules) / sizeof(modules[0]); ++i)
    {
        const struct dsp_module *module = &modules[i];
        int in_module = pc - module->base < module->size;
        contains_pc |= in_module;
#ifdef PORT_MSDSP_INDEPENDENT_LIFTS
        /* A neighbouring ELF may use this LS address for its own BSS.
         * Older combined lifts can bind across modules and require all three. */
        if (!in_module)
            continue;
#endif
        const uint8_t *reference = port_msdsp_bytes + (module->base - DSP_BASE);
        if (memcmp(ctx->ls + module->base, reference, module->size) != 0)
            return 0;
    }
    return contains_pc;
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
    if (pc < DSP_BASE || pc - DSP_BASE >= sizeof(port_msdsp_bytes) ||
        !ctx || !ctx->ls || ctx->image_id != 41)
        return NULL;
    if (!native_enabled)
        return NULL;
    port_spu_fn fn = spu_lookup(pc, DSP_IMAGE);
    if (!fn || !matches(ctx, pc))
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
