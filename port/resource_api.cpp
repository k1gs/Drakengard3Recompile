#include "ppu_recomp.h"
#include "ps3emu/nid.h"
#include <array>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>

extern "C" unsigned char* vm_base;
extern "C" void port_fs_host_path(char*, size_t, const char*);
extern "C" void ps3_hle_register_ctx(uint32_t, const char*, void (*)(ppu_context*));
extern "C" int edat_is_npd(const char*);
extern "C" int edat_decrypt_file_with_klicensee(const char*, const char*, const uint8_t*);
extern "C" const char* edat_resolve(const char*, char*, size_t);

static std::mutex resource_mutex;
static std::map<std::string, std::string> decoded_paths;
static unsigned cache_counter;
constexpr uint32_t invalid_param = 0x80029502, no_entitlement = 0x80029513;
constexpr uint32_t bad_format = 0x80029515, not_found = 0x80010006;
static void result(ppu_context* ctx, uint32_t value)
{ ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)value; }

static void is_available(ppu_context* ctx)
{
    const uint64_t key_ea = ctx->gpr[3], path_ea = ctx->gpr[4];
    if (key_ea > 0xFFFFFFF0ull || !path_ea || path_ea > 0xFFFFFC00ull) {
        result(ctx,invalid_param); return;
    }
    const char* guest = (const char*)vm_base+(uint32_t)path_ea;
    if (!memchr(guest, 0, 1024)) { result(ctx,invalid_param); return; }
    char host[1100];
    port_fs_host_path(host, sizeof host, guest);
    std::error_code ec;
    if (!std::filesystem::is_regular_file(host, ec)) { result(ctx,not_found); return; }
    if (!edat_is_npd(host)) { ctx->gpr[3] = 0; return; }

    std::lock_guard<std::mutex> lock(resource_mutex);
    const char* root = getenv("PS3_EDAT_CACHE");
    if (!root || !*root) {
        fprintf(stderr, "[resource-api] PS3_EDAT_CACHE must name a separate working cache\n");
        result(ctx,bad_format); return;
    }
    std::filesystem::create_directories(root, ec);
    if (ec) { result(ctx,bad_format); return; }
    std::filesystem::path output;
    do { output = std::filesystem::path(root)/("edat-"+std::to_string(++cache_counter)+".dec"); }
    while (std::filesystem::exists(output, ec) && !ec);
    if (ec) { result(ctx,bad_format); return; }

    std::array<uint8_t,16> supplied{};
    if (key_ea) memcpy(supplied.data(), vm_base+(uint32_t)key_ea, supplied.size());
    const std::string destination = output.string();
    int rc = edat_decrypt_file_with_klicensee(host, destination.c_str(), supplied.data());
    // Keep the API argument ephemeral; never print it or persist a key sidecar.
    volatile uint8_t* wipe = supplied.data();
    for (unsigned i = 0; i < supplied.size(); ++i) wipe[i] = 0;
    if (rc) {
        std::filesystem::remove(output,ec);
        fprintf(stderr, "[resource-api] verification/decode failed (%d): %s\n", rc, guest);
        result(ctx,(rc == -13 || rc == -14 || rc == -11) ? no_entitlement : bad_format);
        return;
    }
    decoded_paths[host] = destination;
    fprintf(stderr, "[resource-api] verified license-3 resource available: %s\n", guest);
    ctx->gpr[3] = 0;
}

extern "C" const char* port_edat_resolve(const char* host, char* out, size_t capacity)
{
    {
        std::lock_guard<std::mutex> lock(resource_mutex);
        auto found = decoded_paths.find(host);
        if (found != decoded_paths.end()) {
            if (found->second.size()+1 > capacity) return host;
            memcpy(out, found->second.c_str(), found->second.size()+1);
            return out;
        }
    }
    return edat_resolve(host, out, capacity);
}

extern "C" void port_register_resource_api()
{
    ps3_hle_register_ctx(ps3_compute_nid("sceNpDrmIsAvailable2"), "sceNpDrmIsAvailable2", is_available);
    ps3_hle_register_ctx(ps3_compute_nid("sceNpDrmIsAvailable"), "sceNpDrmIsAvailable", is_available);
}
