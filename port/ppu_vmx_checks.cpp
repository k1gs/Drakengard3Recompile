#include "ppu_recomp.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <climits>

extern "C" void port_vpkswss(ppu_context* ctx,unsigned dst,unsigned left,unsigned right)
{
    // Lifted vectors store architectural lanes as big-endian bytes. Read all
    // sources before storing: audio conversion uses dst == left.
    uint8_t result[16];bool saturated=false;
    for(unsigned i=0;i<8;i++) {
        const uint8_t* p=ctx->vr[i<4?left:right].b+(i%4)*4;
        uint32_t bits=(uint32_t(p[0])<<24)|(uint32_t(p[1])<<16)|(uint32_t(p[2])<<8)|p[3];
        int32_t value;std::memcpy(&value,&bits,4);
        if(value>INT16_MAX) { value=INT16_MAX;saturated=true; }
        else if(value<INT16_MIN) { value=INT16_MIN;saturated=true; }
        uint16_t packed=static_cast<uint16_t>(value);
        result[2*i]=static_cast<uint8_t>(packed>>8);result[2*i+1]=static_cast<uint8_t>(packed);
    }
    std::memcpy(ctx->vr[dst].b,result,16);
    if(saturated) ctx->vscr|=1u; // VSCR.SAT is sticky; preserve all other bits.
}

extern "C" int port_check_vpkswss()
{
    const int32_t cases[2][8]={
        {-32769,-32768,-1,32767,32768,0,INT32_MIN,INT32_MAX},
        {-32768,-1,0,32767,-30000,-2,2,30000}};
    const int16_t expected[2][8]={
        {-32768,-32768,-1,32767,32767,0,-32768,32767},
        {-32768,-1,0,32767,-30000,-2,2,30000}};
    bool ok=true;unsigned checked=0;
    for(unsigned kind=0;kind<2;kind++) for(unsigned alias=0;alias<3;alias++) for(unsigned sticky=0;sticky<2;sticky++) {
        ppu_context ctx{};ctx.vscr=0x10000u|sticky;
        for(unsigned i=0;i<8;i++) {
            uint32_t value=static_cast<uint32_t>(cases[kind][i]);
            for(unsigned b=0;b<4;b++) ctx.vr[i<4?1:2].b[(i%4)*4+b]=static_cast<uint8_t>(value>>(24-8*b));
        }
        unsigned dst=alias==0?0:alias;
        port_vpkswss(&ctx,dst,1,2);
        for(unsigned i=0;i<8;i++) {
            uint16_t value=(uint16_t(ctx.vr[dst].b[i*2])<<8)|ctx.vr[dst].b[i*2+1];
            if(value!=static_cast<uint16_t>(expected[kind][i])) ok=false;
        }
        if(ctx.vscr!=(0x10000u|sticky|(kind==0?1u:0u))) ok=false;
        checked++;
    }
    fprintf(stderr,"[compat-check] vpkswss BE lanes, signed boundaries, dst alias, sticky SAT: %u cases %s\n",checked,ok?"PASS":"FAIL");
    return ok?0:1;
}
