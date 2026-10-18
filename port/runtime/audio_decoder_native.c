#include "spu_context.h"

typedef void (*port_spu_fn)(spu_context *);
extern port_spu_fn spu_lookup(uint32_t, int);
extern void spu_begin_image(int);

#ifdef PORT_WITH_AUDIO_DECODER
#include "audio_decoder_bytes.h"
static int native_enabled;
#endif

void port_audio_decoder_register(void)
{
#ifdef PORT_WITH_AUDIO_DECODER
    const char *e = getenv("PORT_AUDIO_DECODER_NATIVE");
    native_enabled = e && e[0] == '1';
    if (!native_enabled) return;
    spu_begin_image(182);
    port_audio_spu_recomp_register();
#endif
}

port_spu_fn port_audio_decoder_lookup(const spu_context *ctx, uint32_t pc)
{
#ifdef PORT_WITH_AUDIO_DECODER
    /* Only this callback's entry ABI has a real-input differential fixture.
     * Internal direct calls stay compiled; other entry points retain their
     * existing dispatch path until independently verified. */
    if (!native_enabled || !ctx || !ctx->ls || ctx->image_id != 41 || pc != 0x21D20)
        return NULL;
    port_spu_fn fn = spu_lookup(pc, 182);
    if (!fn || memcmp(ctx->ls + PORT_AUDIO_BASE, port_audio_bytes, sizeof(port_audio_bytes)))
        return NULL;
    static _Thread_local int announced;
    if (!announced) {
        announced = 1;
        fprintf(stderr, "[audio-native] complete decoder bytes matched; image=41 pc=%05X\n", pc);
    }
    return fn;
#else
    (void)ctx; (void)pc;
    return NULL;
#endif
}
