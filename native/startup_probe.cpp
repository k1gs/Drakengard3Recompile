// Bounded, single-threaded validation harness. No PS3 API implementation.
#include "ppu_recomp.h"
#include "probe_config.h"
#include <windows.h>
#include <bcrypt.h>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

struct Stop { const char* kind; uint64_t address; };
struct Range { uint64_t begin, end; };
static std::vector<uint8_t> memory(32*1024*1024);
static std::vector<Range> mapped;
extern "C" {
uint8_t* vm_base = memory.data();
PPU_THREAD_LOCAL void (*g_trampoline_fn)(void*) = nullptr;
}
static void bounds(uint64_t a, uint64_t n) {
    for (const auto& r : mapped) if (a >= r.begin && a < r.end && n <= r.end-a) return;
    throw Stop{"unmapped-memory", a};
}
static uint64_t read_be(uint64_t a, unsigned n) {
    bounds(a,n); uint64_t v=0;
    for (unsigned i=0;i<n;++i) v=(v<<8)|memory[a+i];
    return v;
}
static void write_be(uint64_t a, uint64_t v, unsigned n) {
    bounds(a,n);
    for (unsigned i=0;i<n;++i) memory[a+n-1-i]=uint8_t(v>>(8*i));
}
extern "C" {
uint8_t vm_read8(uint64_t a) { return uint8_t(read_be(a,1)); }
uint16_t vm_read16(uint64_t a) { return uint16_t(read_be(a,2)); }
uint32_t vm_read32(uint64_t a) { return uint32_t(read_be(a,4)); }
uint64_t vm_read64(uint64_t a) { return read_be(a,8); }
void vm_write8(uint64_t a,uint8_t v) { write_be(a,v,1); }
void vm_write16(uint64_t a,uint16_t v) { write_be(a,v,2); }
void vm_write32(uint64_t a,uint32_t v) { write_be(a,v,4); }
void vm_write64(uint64_t a,uint64_t v) { write_be(a,v,8); }
void ps3_hle_call(unsigned int nid,ppu_context*) { throw Stop{"hle-import",nid}; }
void ps3_indirect_call(ppu_context* ctx) { throw Stop{"indirect-frontier",ctx->ctr}; }
void lv2_syscall(ppu_context* ctx) { throw Stop{"lv2-frontier",ctx->gpr[11]}; }
}
[[noreturn]] void probe_frontier(unsigned long long target) { throw Stop{"direct-frontier",target}; }

static uint64_t file_be(const std::vector<uint8_t>& f,uint64_t a,unsigned n) {
    if (a > f.size() || n > f.size()-a) throw std::runtime_error("ELF header outside file");
    uint64_t v=0; for (unsigned i=0;i<n;++i) v=(v<<8)|f[a+i]; return v;
}
static std::string sha256(const std::vector<uint8_t>& data) {
    BCRYPT_ALG_HANDLE provider=nullptr;
    if (BCryptOpenAlgorithmProvider(&provider,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0) throw std::runtime_error("SHA256 provider unavailable");
    uint8_t digest[32];
    const auto status=BCryptHash(provider,nullptr,0,const_cast<uint8_t*>(data.data()),ULONG(data.size()),digest,sizeof(digest));
    BCryptCloseAlgorithmProvider(provider,0);
    if (status<0) throw std::runtime_error("SHA256 failed");
    std::ostringstream text; text<<std::hex<<std::setfill('0');
    for (auto b:digest) text<<std::setw(2)<<unsigned(b);
    return text.str();
}
static void load_elf(const char* path) {
    std::ifstream stream(path,std::ios::binary);
    if (!stream) throw std::runtime_error("Cannot open ELF");
    stream.seekg(0,std::ios::end);
    if (stream.tellg()!=PROBE_ELF_SIZE) throw std::runtime_error("ELF size mismatch");
    stream.seekg(0);
    std::vector<uint8_t> file((std::istreambuf_iterator<char>(stream)),{});
    if (sha256(file)!=PROBE_ELF_SHA) throw std::runtime_error("ELF SHA256 mismatch");
    if (file_be(file,0,4)!=0x7f454c46 || file_be(file,4,2)!=0x0202 || file_be(file,18,2)!=21)
        throw std::runtime_error("Expected ELF64 BE PPU");
    const auto phoff=file_be(file,32,8), count=file_be(file,56,2), step=file_be(file,54,2);
    if (count>1024 || step!=56) throw std::runtime_error("Invalid program headers");
    for (uint64_t i=0;i<count;++i) {
        const auto p=phoff+i*step;
        if (file_be(file,p,4)!=1) continue;
        const auto off=file_be(file,p+8,8), va=file_be(file,p+16,8);
        const auto size=file_be(file,p+32,8), mem=file_be(file,p+40,8);
        if (size>mem || off>file.size() || size>file.size()-off || va>memory.size() || mem>memory.size()-va)
            throw std::runtime_error("Segment outside probe memory");
        for (const auto& r:mapped) if (va<r.end && r.begin<va+mem) throw std::runtime_error("Overlapping segments");
        std::memcpy(memory.data()+va,file.data()+off,size);
        mapped.push_back({va,va+mem});
    }
    mapped.push_back({0x1e00000,0x1e10000});
    if (vm_read32(PROBE_ENTRY_OPD)!=PROBE_ENTRY || vm_read32(PROBE_ENTRY_OPD+4)!=PROBE_TOC)
        throw std::runtime_error("Entry OPD differs from observed build");
}
static void require(bool value,const char* message) { if (!value) throw std::runtime_error(message); }

int main(int argc,char** argv) {
    if (argc<2) { std::fprintf(stderr,"Usage: startup_probe EBOOT.elf [variant=0..3] [unmapped-toc|wrong-register|table-helper|reference-boot]\n"); return 2; }
    try {
        load_elf(argv[1]);
        const unsigned variant=argc>2?std::stoul(argv[2]):0;
        require(variant<4,"Variant must be 0..3");
        const std::string fault=argc>3?argv[3]:"";
        require(fault.empty() || fault=="unmapped-toc" || fault=="wrong-register" || fault=="table-helper" || fault=="reference-boot","Unknown probe mode");
        ppu_context ctx{};
        for (unsigned i=0;i<32;++i) ctx.gpr[i]=0x10000000+i*0x1111+variant;
        const uint64_t initial_sp=0x1e08000+variant*0x1000;
        ctx.gpr[1]=initial_sp;
        // Synthetic register inputs, deliberately distinct. Not an asserted PS3 boot ABI.
        ctx.gpr[3]=1+variant; ctx.gpr[4]=0x1e02000+variant*0x100;
        ctx.gpr[5]=0x1e03000+variant*0x100; ctx.gpr[6]=variant;
        ctx.gpr[7]=0x11223344+variant; ctx.gpr[8]=0x1aabbccddULL+variant;
        ctx.gpr[9]=0x212345678ULL+variant; ctx.gpr[10]=0x387654321ULL+variant;
        ctx.gpr[12]=0xcafef00d+variant;
        if (fault=="reference-boot") {
            // Reference-shaped scalar inputs from pinned RPCS3 loader and ELF PT_TLS.
            // Local stack/argv addresses and the overall process model remain synthetic.
            ctx.gpr[7]=0x1000000;
            ctx.gpr[8]=PROBE_TLS_VADDR;
            ctx.gpr[9]=PROBE_TLS_FILESZ;
            ctx.gpr[10]=PROBE_TLS_MEMSZ;
            ctx.gpr[11]=PROBE_ENTRY_OPD;
            ctx.gpr[12]=0x100000;
            bounds(PROBE_TLS_VADDR,PROBE_TLS_FILESZ);
        }
        const auto before=ctx;
        if (fault=="table-helper") {
            // Exercise the real, self-contained 253-iteration initializer separately.
            // It is not called past the TLS barrier in the startup-prefix test.
            ctx.gpr[2]=PROBE_TOC;
            const auto base=vm_read32(PROBE_TOC-0x1838);
            bounds(base,0x800);
            std::memset(memory.data()+base,variant?0x5a:0xa5,0x800);
            func_00EA2EE4(&ctx);
            require(ctx.ctr==0,"Table initializer did not exhaust CTR");
            for (unsigned i=0;i<256;++i) {
                require(vm_read32(base+4*i)==(i<3?i:0xffffffffu),"Initializer first table differs");
                require(vm_read32(base+0x400+4*i)==0,"Initializer second table differs");
            }
            require(memory[base+4]==0 && memory[base+7]==1 && memory[base+8]==0 && memory[base+11]==2,"Initializer IDs are not stored big-endian");
            std::printf("PASS table_helper variant=%u iterations=253 base=0x%llx\n",variant,base);
            return 0;
        }
        const auto global=vm_read32(PROBE_TOC-0x7ffc);
        if (fault=="unmapped-toc") vm_write32(PROBE_ENTRY_OPD+4,0x7fff0000);
        if (fault=="wrong-register") ctx.gpr[8]^=1;
        try {
            func_00A8A020(&ctx);
            while (g_trampoline_fn) { auto fn=g_trampoline_fn; g_trampoline_fn=nullptr; fn(&ctx); }
            throw std::runtime_error("Unexpected startup return before barrier");
        } catch (const Stop& stop) {
            std::printf("STOP kind=%s value=0x%llx lr=0x%llx sp=0x%llx toc=0x%llx\n",stop.kind,stop.address,ctx.lr,ctx.gpr[1],ctx.gpr[2]);
            require(std::strcmp(stop.kind,"hle-import")==0 && stop.address==0x744680a2,"Unexpected stop frontier");
            require(ctx.lr==0xa8a180,"Wrong first-import LR");
            require(ctx.gpr[1]==initial_sp-0x110,"Wrong entry/CRT stack frame");
            require(ctx.gpr[2]==PROBE_TOC,"Wrong entry TOC");
            require(ctx.gpr[3]==before.gpr[7] && ctx.gpr[4]==uint32_t(before.gpr[8]) && ctx.gpr[5]==uint32_t(before.gpr[9]) && ctx.gpr[6]==uint32_t(before.gpr[10]),"TLS arguments differ from instruction semantics");
            require(ctx.gpr[14]==0 && ctx.gpr[31]==before.gpr[3] && ctx.gpr[28]==before.gpr[4] && ctx.gpr[26]==before.gpr[5] && ctx.gpr[30]==before.gpr[6] && ctx.gpr[29]==before.gpr[12],"CRT register saves differ");
            require(vm_read64(initial_sp-0x70)==0 && vm_read64(initial_sp-0x110)==initial_sp-0x70,"Wrong stack back chain");
            require(vm_read64(initial_sp-0x60)==0xa8a040,"Wrong saved return address");
            // Check raw guest bytes independently of read/write helper round trips.
            bounds(global,4);
            for (unsigned i=0;i<4;++i) require(memory[global+i]==uint8_t(before.gpr[12]>>(24-8*i)),"Wrong global store byte order");
            std::printf("PASS variant=%u entry_to_first_import synthetic_boot=true hle_implemented=false\n",variant);
            if (fault=="reference-boot") std::printf("PASS reference_boot_scalars tls=0x%llx filesz=0x%llx memsz=0x%llx\n",ctx.gpr[4],ctx.gpr[5],ctx.gpr[6]);
        }
        return 0;
    } catch (const std::exception& e) { std::fprintf(stderr,"FAIL %s\n",e.what()); return 1; }
      catch (const Stop& stop) { std::fprintf(stderr,"FAIL stop %s 0x%llx\n",stop.kind,stop.address); return 1; }
}
