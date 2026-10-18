#include "spu_context.h"

#ifdef PORT_WITH_AUDIO_DECODER
#include "audio_decoder_fixture.h"
typedef void (*port_spu_fn)(spu_context *);
extern void port_audio_decoder_register(void);
extern port_spu_fn port_audio_decoder_lookup(const spu_context *, uint32_t);
extern void spu_begin_image(int);
extern void spu_register_function(uint32_t, port_spu_fn);
extern uint32_t spu_interp_run_until(spu_context *, uint32_t, uint32_t);
extern void port_spu_set_test_clock(uint64_t (*)(void));
static uint64_t fixture_ns;
static uint64_t fixture_clock(void) { fixture_ns += 1000; return fixture_ns; }
static void fixture_return(spu_context *ctx) { ctx->pc = 0x3FFF0; }
#endif

int port_check_audio_decoder(void)
{
#ifdef PORT_WITH_AUDIO_DECODER
    _putenv_s("PORT_AUDIO_DECODER_NATIVE", "1");
    port_audio_decoder_register();
    spu_context *ctx = (spu_context *)calloc(1, sizeof(*ctx));
    uint8_t *result = (uint8_t *)malloc(SPU_LS_SIZE);
    if (!ctx || !result) { free(ctx); free(result); return 2; }
    spu_context_init(ctx, 0);
    memcpy(ctx->ls, port_audio_input + 2048, SPU_LS_SIZE);
    memcpy(ctx->gpr, port_audio_input, 2048);
    ctx->image_id = 41;
    int identity = port_audio_decoder_lookup(ctx, 0x21D20) != NULL;
    ctx->ls[0x1C100] ^= 1;
    identity = identity && !port_audio_decoder_lookup(ctx, 0x21D20);
    ctx->ls[0x1C100] ^= 1;
    ctx->image_id = 200;
    identity = identity && !port_audio_decoder_lookup(ctx, 0x21D20);
    ctx->image_id = 41;
    /* Only the return address is replaced with a private fixture sentinel.
     * Decoder arguments, all other registers and every LS byte are captured inputs. */
    ctx->gpr[0]._u32[0] = 0x3FFF0;
    u128 initial[128], after[128];
    memcpy(initial, ctx->gpr, sizeof(initial));
    spu_begin_image(41);
    spu_register_function(0x3FFF0, fixture_return);
    ctx->pc = 0x21D20; ctx->status = SPU_STATUS_RUNNING;
    /* Decrementer values are observable profiling outputs. Supply both runs
     * the same clock sequence instead of ignoring any differing result bytes. */
    fixture_ns=1000000;
    port_spu_set_test_clock(fixture_clock);
    port_spu_fn fn = port_audio_decoder_lookup(ctx, ctx->pc);
    unsigned calls = 0;
    while (fn && ctx->pc != 0x3FFF0 && calls++ < 1000000) {
        g_spu_trampoline_fn = NULL;
        fn(ctx);
        fn = g_spu_trampoline_fn;
    }
    int returned = ctx->pc == 0x3FFF0;
    memcpy(after, ctx->gpr, sizeof(after));
    memcpy(result, ctx->ls, SPU_LS_SIZE);
    memcpy(ctx->gpr, initial, sizeof(initial));
    memcpy(ctx->ls, port_audio_input + 2048, SPU_LS_SIZE);
    ctx->image_id = 200; ctx->pc = 0x21D20; ctx->status = SPU_STATUS_RUNNING;
    ctx->dec_base_ns=0;ctx->decrementer=0;
    fixture_ns=1000000;
    g_spu_trampoline_fn = NULL;
    spu_interp_run_until(ctx, 0x21D20, 0x3FFF0);
    port_spu_set_test_clock(NULL);
    int registers = !memcmp(after, ctx->gpr, sizeof(after));
    int memory = !memcmp(result, ctx->ls, SPU_LS_SIZE);
    if (!registers) {
        unsigned shown=0;
        for(unsigned r=0;r<128 && shown<8;r++) if(memcmp(&after[r],&ctx->gpr[r],16)) {
            fprintf(stderr,"[audio-diff] r%u native=%08X/%08X/%08X/%08X interp=%08X/%08X/%08X/%08X\n",r,
                after[r]._u32[0],after[r]._u32[1],after[r]._u32[2],after[r]._u32[3],
                ctx->gpr[r]._u32[0],ctx->gpr[r]._u32[1],ctx->gpr[r]._u32[2],ctx->gpr[r]._u32[3]);shown++;
        }
    }
    if (!memory) {
        unsigned count=0,first=SPU_LS_SIZE,last=0;
        for(unsigned i=0;i<SPU_LS_SIZE;i++) if(result[i]!=ctx->ls[i]) { if(i<first) first=i;last=i;count++; }
        fprintf(stderr,"[audio-diff] LS count=%u first=%05X last=%05X\n",count,first,last);
    }
    int ok = identity && returned && ctx->pc == 0x3FFF0 && registers && memory;
    fprintf(stderr, "[compat-check] audio decoder real inputs: identity=%d returned=%d registers=%d LS=%d %s\n",
            identity, returned, registers, memory, ok ? "PASS" : "FAIL");
    free(result); free(ctx);
    return ok ? 0 : 1;
#else
    fprintf(stderr, "[compat-check] audio decoder lift missing\n");
    return 2;
#endif
}
