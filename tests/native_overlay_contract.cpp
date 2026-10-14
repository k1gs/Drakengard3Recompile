#include "../port/spu_elf_view.h"
#include <vector>
#include <fstream>
#include <iterator>
#include <cstdio>

static void put16(std::vector<uint8_t>& b,unsigned o,uint16_t v) { b[o]=v>>8; b[o+1]=v; }
static void put32(std::vector<uint8_t>& b,unsigned o,uint32_t v) { b[o]=v>>24; b[o+1]=v>>16; b[o+2]=v>>8; b[o+3]=v; }
static std::vector<uint8_t> image()
{
    std::vector<uint8_t> b(512);
    memcpy(b.data(),"\x7f" "ELF",4); b[4]=1; b[5]=2;
    put16(b,18,23); put32(b,24,0x3000); put32(b,28,52);
    put16(b,42,32); put16(b,44,2);
    put32(b,52,1); put32(b,56,256); put32(b,60,0x3000);
    put32(b,68,16); put32(b,72,16); put32(b,76,5);
    put32(b,84,1); put32(b,88,272); put32(b,92,0x3080);
    put32(b,100,16); put32(b,104,128); put32(b,108,6);
    return b;
}

int main(int argc,char** argv)
{
    using K=port::SpuImageKind;
    unsigned passed=0;
    auto check=[&](const std::vector<uint8_t>& b,K expected) {
        bool ok=port::inspect_spu_image(b.data(),b.size())==expected;
        if(ok) ++passed;
        return ok;
    };
    auto b=image(); if(!check(b,K::resident_only)) return 1;
    put32(b,92,0x3008); if(!check(b,K::overlays)) return 2;
    b=image(); put16(b,18,21); if(!check(b,K::malformed)) return 3;
    b=image(); b.resize(80); if(!check(b,K::malformed)) return 4;
    b=image(); put32(b,104,0xFFFFFFFF); if(!check(b,K::malformed)) return 5;
    b=image(); put32(b,32,116); put16(b,46,40); put16(b,48,2); put16(b,50,1);
    put32(b,156,1); put32(b,160,3); put32(b,172,200); put32(b,176,8);
    memcpy(b.data()+200,"\0.ovtab\0",8);
    if(!check(b,K::overlays)) return 6;
    b=image(); put32(b,28,0xFFFFFFF0); if(!check(b,K::malformed)) return 7;
    printf("SPU resident-image contract: %u/7 PASS\n",passed);
    if(argc==2) {
        std::ifstream f(argv[1],std::ios::binary);
        if(!f) return 8;
        std::vector<uint8_t> real((std::istreambuf_iterator<char>(f)),{});
        if(port::inspect_spu_image(real.data(),real.size())!=K::resident_only) return 9;
        printf("Actual supplied SPU image: resident-only, no overlays\n");
    }
    return 0;
}
