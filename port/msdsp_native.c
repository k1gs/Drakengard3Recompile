#include "spu_context.h"

typedef void (*port_spu_fn)(spu_context*);
extern port_spu_fn spu_lookup(uint32_t, int);
extern void spu_begin_image(int);
extern void spu_register_function(uint32_t,port_spu_fn);
extern uint32_t spu_interp_run_until(spu_context*,uint32_t,uint32_t);

#ifdef PORT_WITH_MSDSP
#include "msdsp_bytes.h"
enum { DSP_IMAGE = 181, DSP_BASE = 0x37000 };
static int native_enabled; /* Set before starting any guest threads. */
static void fixture_return(spu_context* ctx) { (void)ctx; }

static int matches(const spu_context* ctx)
{
    /* Validate every module byte, not just the first plugin's GUID. Heap
     * addresses vary; the compiled PIC code is valid only at these LS bases.
     * Padding between modules is not executable and need not match. */
    return ctx && ctx->ls && ctx->image_id == 41 &&
        !memcmp(ctx->ls+0x37000,port_msdsp_bytes,0x1F80) &&
        !memcmp(ctx->ls+0x39000,port_msdsp_bytes+0x2000,0x890) &&
        !memcmp(ctx->ls+0x39900,port_msdsp_bytes+0x2900,0x420);
}
#endif

void port_msdsp_register(void)
{
#ifdef PORT_WITH_MSDSP
    const char* e=getenv("PORT_MSDSP_NATIVE");
    native_enabled=!(e && e[0]=='0');
    spu_begin_image(DSP_IMAGE);
    port_msdsp_spu_recomp_register();
#endif
}

port_spu_fn port_msdsp_lookup(const spu_context* ctx,uint32_t pc)
{
#ifdef PORT_WITH_MSDSP
    if(!ctx || ctx->image_id!=41 || pc<DSP_BASE || pc>=0x39D20) return NULL;
    if(!native_enabled) return NULL;
    port_spu_fn fn=spu_lookup(pc,DSP_IMAGE);
    if(!fn || !matches(ctx)) return NULL;
    static _Thread_local int announced;
    if(!announced) {
        announced=1;
        fprintf(stderr,"[msdsp-native] full module match; compiled DSP entry=%05X image=%d\n",pc,ctx->image_id);
    }
    return fn;
#else
    (void)ctx;(void)pc;return NULL;
#endif
}

int port_check_msdsp(void)
{
#ifdef PORT_WITH_MSDSP
    spu_context* ctx=(spu_context*)calloc(1,sizeof(*ctx));
    if(!ctx) return 2;
    spu_context_init(ctx,0);
    ctx->image_id=41;
    memcpy(ctx->ls+DSP_BASE,port_msdsp_bytes,sizeof(port_msdsp_bytes));
    port_msdsp_register();
    int ok=port_msdsp_lookup(ctx,0x37EA8)!=NULL;
    const uint32_t probes[]={0x37020,0x39020,0x39920,0x38F7F,0x3988F,0x39D1F};
    for(unsigned i=0;i<sizeof(probes)/sizeof(probes[0]);i++) {
        ctx->ls[probes[i]]^=1;
        if(port_msdsp_lookup(ctx,0x37EA8)) ok=0;
        ctx->ls[probes[i]]^=1;
    }
    ctx->image_id=42;
    if(port_msdsp_lookup(ctx,0x37EA8)) ok=0;
    ctx->image_id=41;
    if(port_msdsp_lookup(ctx,0x36FFC) || port_msdsp_lookup(ctx,0x39D20)) ok=0;
    if(!port_msdsp_lookup(ctx,0x37EA8)) ok=0;
    fprintf(stderr,"[compat-check] DSP full-byte identity, all modules, owner and bounds: %s\n",ok?"PASS":"FAIL");
    /* Execute a real PIC callback initializer both ways. It derives function
     * addresses from its relocated link register and writes the host's table.
     * The private return stub exists only in this fixture process. */
    uint8_t* initial=(uint8_t*)malloc(SPU_LS_SIZE);
    uint8_t* result=(uint8_t*)malloc(SPU_LS_SIZE);
    if(!initial || !result) {free(initial);free(result);free(ctx);return 2;}
    ctx->gpr[0]._u32[0]=0x36000;
    ctx->gpr[1]._u32[0]=0x20000;
    ctx->gpr[3]._u32[0]=0x1000;
    u128 before[128],after[128];
    memcpy(before,ctx->gpr,sizeof(before));memcpy(initial,ctx->ls,SPU_LS_SIZE);
    spu_begin_image(41);spu_register_function(0x36000,fixture_return);
    port_spu_fn fn=port_msdsp_lookup(ctx,0x37018);
    ctx->pc=0x37018;
    unsigned calls=0;
    while(fn && ctx->pc!=0x36000 && calls++<1000) {
        g_spu_trampoline_fn=NULL;fn(ctx);fn=g_spu_trampoline_fn;
    }
    int executed=ctx->pc==0x36000;
    memcpy(after,ctx->gpr,sizeof(after));memcpy(result,ctx->ls,SPU_LS_SIZE);
    memcpy(ctx->gpr,before,sizeof(before));memcpy(ctx->ls,initial,SPU_LS_SIZE);
    ctx->image_id=200;ctx->pc=0x37018;g_spu_trampoline_fn=NULL;
    spu_interp_run_until(ctx,0x37018,0x36000);
    int same=executed && ctx->pc==0x36000 && !memcmp(after,ctx->gpr,sizeof(after)) &&
        !memcmp(result,ctx->ls,SPU_LS_SIZE);
    fprintf(stderr,"[compat-check] DSP PIC initializer native/interpreter all registers and LS: %s\n",same?"PASS":"FAIL");
    ok=ok && same;free(initial);free(result);
    free(ctx);return ok?0:1;
#else
    fprintf(stderr,"[compat-check] DSP lift missing\n");return 2;
#endif
}
