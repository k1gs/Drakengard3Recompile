#include "spu_context.h"
#include <cstdio>
#include <cstdlib>
#include <memory>

extern "C" void spu_indirect_branch(spu_context*);
extern "C" uint32_t spu_interp_run(spu_context*,uint32_t);

extern "C" int port_check_spu_interpreter_halt()
{
    auto ctx=std::make_unique<spu_context>();
    spu_context_init(ctx.get(),0);
    ctx->image_id=200;
    // heqi r11,0: r11 starts at zero, so execution must stop with exit 8.
    constexpr uint32_t word=0x7F000580;
    for(unsigned i=0;i<4;i++) ctx->ls[0x37000+i]=(uint8_t)(word>>(24-i*8));
    spu_interp_run(ctx.get(),0x37000);
    fprintf(stderr,"[compat-check] interpreter HALT incorrectly returned\n");
    return 1;
}

extern "C" int port_check_spu_return()
{
    // A runtime-loaded callee returns to the middle of a lifted host function.
    // Continuing interpretation there executes the caller a second time.
    _putenv_s("SPU_INTERP_UNLIFTED", "1");
    auto ctx=std::make_unique<spu_context>();
    spu_context_init(ctx.get(),0);
    ctx->image_id=200;
    constexpr uint32_t entry=0x37000,ret=0x10004;
    auto word=[&](uint32_t at,uint32_t value) {
        ctx->ls[at]=(uint8_t)(value>>24); ctx->ls[at+1]=(uint8_t)(value>>16);
        ctx->ls[at+2]=(uint8_t)(value>>8); ctx->ls[at+3]=(uint8_t)value;
    };
    word(entry,0x40800383); // il r3,7
    word(entry+4,0x35000000); // bi r0
    word(ret,0x40803183); // il r3,99 -- caller continuation must not execute
    word(ret+4,0x00002000); // stop 0x2000
    ctx->gpr[0]._u32[0]=ret;
    ctx->pc=entry;
    spu_indirect_branch(ctx.get());
    bool ok=ctx->pc==ret && ctx->gpr[3]._u32[0]==7 && ctx->status==SPU_STATUS_RUNNING;
    fprintf(stderr,"[compat-check] unlifted callee returns before host continuation: %s (pc=%05X r3=%u status=%X)\n",
            ok?"PASS":"FAIL",ctx->pc,ctx->gpr[3]._u32[0],ctx->status);
    return ok?0:1;
}
