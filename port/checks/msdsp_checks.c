#include "spu_context.h"

typedef void (*port_spu_fn)(spu_context *);
extern void port_msdsp_register(void);
extern port_spu_fn port_msdsp_lookup(const spu_context *, uint32_t);
extern void spu_begin_image(int);
extern void spu_register_function(uint32_t, port_spu_fn);
extern uint32_t spu_interp_run_until(spu_context *, uint32_t, uint32_t);

#ifdef PORT_WITH_MSDSP
#include "msdsp_bytes.h"
enum
{
    DSP_BASE = 0x37000,
    RETURN_PC = 0x36000
};
static void fixture_return(spu_context *ctx)
{
    (void)ctx;
}

static int check_identity(spu_context *ctx)
{
    static const struct {
        uint32_t entry;
        uint32_t probes[3];
    } plugins[] = {
        {0x37EA8, {0x37000, 0x37020, 0x38F7F}},
        {0x39630, {0x39000, 0x39020, 0x3988F}},
        {0x39910, {0x39900, 0x39920, 0x39D1F}}
    };
    for (size_t changed = 0; changed < 3; ++changed)
    {
        for (size_t byte = 0; byte < 3; ++byte)
        {
            uint32_t address = plugins[changed].probes[byte];
            ctx->ls[address] ^= 1;
            for (size_t queried = 0; queried < 3; ++queried)
            {
#ifdef PORT_MSDSP_INDEPENDENT_LIFTS
                int expected = queried != changed;
#else
                int expected = 0;
#endif
                if ((port_msdsp_lookup(ctx, plugins[queried].entry) != NULL) != expected)
                    return 0;
            }
            ctx->ls[address] ^= 1;
            if (!port_msdsp_lookup(ctx, plugins[changed].entry))
                return 0;
        }
    }
    ctx->image_id = 42;
    int wrong_owner = port_msdsp_lookup(ctx, plugins[0].entry) != NULL;
    ctx->image_id = 41;
    return !wrong_owner && !port_msdsp_lookup(ctx, 0x36FFC) &&
           !port_msdsp_lookup(ctx, 0x38F80) && !port_msdsp_lookup(ctx, 0x39890) &&
           !port_msdsp_lookup(ctx, 0x39D20);
}

static int compare_execution(spu_context *native, spu_context *reference, uint32_t entry)
{
    memcpy(reference->gpr, native->gpr, sizeof(native->gpr));
    memcpy(reference->ls, native->ls, SPU_LS_SIZE);
    reference->image_id = 200; /* Unregistered image for the interpreter oracle. */
    native->pc = entry;
    port_spu_fn fn = port_msdsp_lookup(native, entry);
    for (unsigned calls = 0; fn && native->pc != RETURN_PC && calls < 1000; ++calls)
    {
        g_spu_trampoline_fn = NULL;
        fn(native);
        fn = g_spu_trampoline_fn;
    }
    g_spu_trampoline_fn = NULL;
    spu_interp_run_until(reference, entry, RETURN_PC);
    return native->pc == RETURN_PC && reference->pc == RETURN_PC &&
           !memcmp(native->gpr, reference->gpr, sizeof(native->gpr)) &&
           !memcmp(native->ls, reference->ls, SPU_LS_SIZE);
}

static int check_filter(spu_context *ctx, spu_context *reference)
{
    enum { STACK = 0x20000, COEFFICIENTS = 0x8000, INPUT = 0x9000, OUTPUT = 0xA000 };
    /* Enter the second plugin's filter loop with its final eight samples. */
    ctx->gpr[0]._u32[0] = RETURN_PC;
    ctx->gpr[1]._u32[0] = STACK;
    spu_ls_write128(ctx, STACK + 0x20, (u128){._u32 = {504}});
    spu_ls_write128(ctx, STACK + 0x50, (u128){._u32 = {COEFFICIENTS}});
    spu_ls_write128(ctx, STACK + 0x80, (u128){._u32 = {OUTPUT}});
    spu_ls_write128(ctx, STACK + 0x90, (u128){._u32 = {INPUT}});
    spu_ls_write128(ctx, STACK + 0xA0, (u128){0});
    spu_ls_write128(ctx, COEFFICIENTS, (u128){._f32 = {0.5f, 0.25f, -0.125f, 0.0625f}});
    spu_ls_write128(ctx, COEFFICIENTS + 16, (u128){._f32 = {-0.03125f}});
    spu_ls_write128(ctx, INPUT, (u128){._f32 = {2.0f, -4.0f, 1.0f, 0.25f}});
    spu_ls_write128(ctx, INPUT + 16, (u128){._f32 = {3.0f, -0.5f, 0.0f, 1.0f}});
#ifdef PORT_MSDSP_INDEPENDENT_LIFTS
    /* This plugin's BSS overlaps the following plugin's GUID in Chapter 0. */
    spu_ls_write128(ctx, 0x39900, (u128){._u32 = {1, 2, 3, 4}});
#endif
    return compare_execution(ctx, reference, 0x39630) &&
           spu_ls_read128(ctx, OUTPUT)._f32[0] == 1.0f &&
           spu_ls_read128(ctx, STACK + 0x20)._u32[0] == 512;
}
#endif

int port_check_msdsp(void)
{
#ifdef PORT_WITH_MSDSP
    spu_context *ctx = (spu_context *)calloc(1, sizeof(*ctx));
    spu_context *reference = (spu_context *)calloc(1, sizeof(*reference));
    if (!ctx || !reference)
    {
        free(reference);
        free(ctx);
        return 2;
    }
    spu_context_init(ctx, 0);
    spu_context_init(reference, 0);
    ctx->image_id = 41;
    memcpy(ctx->ls + DSP_BASE, port_msdsp_bytes, sizeof(port_msdsp_bytes));
    port_msdsp_register();
    int identity = check_identity(ctx);
    fprintf(stderr, "[compat-check] DSP module identity, owner and bounds: %s\n",
            identity ? "PASS" : "FAIL");
    /* Execute a real PIC callback initializer both ways. It derives function
     * addresses from its relocated link register and writes the host's table.
     * The private return stub exists only in this fixture process. */
    ctx->gpr[0]._u32[0] = RETURN_PC;
    ctx->gpr[1]._u32[0] = 0x20000;
    ctx->gpr[3]._u32[0] = 0x1000;
    spu_begin_image(41);
    spu_register_function(RETURN_PC, fixture_return);
    int initializer = compare_execution(ctx, reference, 0x37018);
    int filter = check_filter(ctx, reference);
    fprintf(stderr,
            "[compat-check] DSP PIC initializer native/interpreter all registers and LS: %s\n",
            initializer ? "PASS" : "FAIL");
    fprintf(stderr, "[compat-check] DSP sample filter native/interpreter all registers and LS: %s\n",
            filter ? "PASS" : "FAIL");
    free(reference);
    free(ctx);
    return identity && initializer && filter ? 0 : 1;
#else
    fprintf(stderr, "[compat-check] DSP lift missing\n");
    return 2;
#endif
}
