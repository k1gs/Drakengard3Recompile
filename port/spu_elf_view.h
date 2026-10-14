#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>

namespace port {
enum class SpuImageKind { malformed, resident_only, overlays };
inline SpuImageKind inspect_spu_image(const uint8_t* p, size_t size)
{
    auto fits = [size](uint64_t off, uint64_t count) { return off <= size && count <= size-off; };
    auto u16 = [p](size_t off) { return (uint16_t)((p[off]<<8)|p[off+1]); };
    auto u32 = [p](size_t off) { return ((uint32_t)p[off]<<24)|((uint32_t)p[off+1]<<16)|((uint32_t)p[off+2]<<8)|p[off+3]; };
    if (!p || size < 52 || memcmp(p,"\x7f" "ELF",4) || p[4]!=1 || p[5]!=2 || u16(18)!=23)
        return SpuImageKind::malformed;
    uint32_t phoff=u32(28), shoff=u32(32);
    uint16_t phsize=u16(42), phnum=u16(44), shsize=u16(46), shnum=u16(48), names=u16(50);
    if (!phnum || phnum>512 || phsize!=32 || !fits(phoff,(uint64_t)phnum*phsize))
        return SpuImageKind::malformed;
    uint32_t starts[512], ends[512]; unsigned loads=0; bool overlay=false, entry=false;
    for(unsigned i=0;i<phnum;i++) {
        size_t o=phoff+(size_t)i*phsize;
        if(u32(o)!=1) continue;
        uint32_t off=u32(o+4), va=u32(o+8), file=u32(o+16), mem=u32(o+20), flags=u32(o+24);
        if(file>mem || !fits(off,file) || va>0x40000 || mem>0x40000-va) return SpuImageKind::malformed;
        if(flags & ~7u) overlay=true; // Unknown overlay-specific flags are not resident-only.
        if((flags&1) && u32(24)>=va && (uint64_t)u32(24)<(uint64_t)va+file) entry=true;
        if(mem) {
            for(unsigned j=0;j<loads;j++) if(va<ends[j] && starts[j]<va+mem) overlay=true;
            starts[loads]=va; ends[loads++]=va+mem;
        }
    }
    if(!loads || !entry) return SpuImageKind::malformed;
    if(shnum) {
        if(shsize!=40 || names>=shnum || !fits(shoff,(uint64_t)shnum*shsize)) return SpuImageKind::malformed;
        size_t st=shoff+(size_t)names*shsize;
        uint32_t str_off=u32(st+16), str_size=u32(st+20);
        if(!fits(str_off,str_size)) return SpuImageKind::malformed;
        for(unsigned i=0;i<shnum;i++) {
            size_t o=shoff+(size_t)i*shsize;
            if(u32(o+4)!=8 && !fits(u32(o+16),u32(o+20))) return SpuImageKind::malformed;
            uint32_t name=u32(o);
            if(name>=str_size) return SpuImageKind::malformed;
            const char* s=(const char*)p+str_off+name;
            const char* end=(const char*)memchr(s,0,str_size-name);
            if(!end) return SpuImageKind::malformed;
            if(end-s>=4 && !memcmp(s,".ovl",4)) overlay=true;
            if(end-s>=6 && !memcmp(s,".ovtab",6)) overlay=true;
        }
    }
    return overlay ? SpuImageKind::overlays : SpuImageKind::resident_only;
}
}
