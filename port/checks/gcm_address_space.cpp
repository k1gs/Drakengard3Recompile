#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>

extern "C" {
extern std::uint8_t* vm_base;
extern std::uint32_t ppu_hle_inject_base;
void vm_write32(std::uint64_t, std::uint32_t);
std::uint32_t vm_read32(std::uint64_t);
void port_gcm_contract_begin(std::uint32_t);
void cellGcm_rsx_process_fifo();
std::int32_t cellGcmMapMainMemory(std::uint32_t, std::uint32_t, std::uint32_t*);
std::int32_t cellGcmMapEaIoAddress(std::uint32_t, std::uint32_t, std::uint32_t);
std::int32_t cellGcmUnmapEaIoAddress(std::uint32_t);
std::int32_t cellGcmIoOffsetToAddress(std::uint32_t, std::uint32_t*);
std::uint32_t port_gcm_io_ea(std::uint32_t);
int rsx_d3d12_backend_init(std::uint32_t, std::uint32_t, const char*);
}

static void method(std::uint32_t fifo, std::uint32_t& at,
                   std::uint32_t subchannel, std::uint32_t reg,
                   std::uint32_t value)
{
    vm_write32(fifo + at, 0x00040000u | (subchannel << 13) | reg);
    vm_write32(fifo + at + 4, value);
    at += 8;
}

extern "C" int port_check_gcm_address_space()
{
    constexpr std::uint32_t fifo = 0x60000000u;
    constexpr std::uint32_t main_a = 0x20000000u;
    constexpr std::uint32_t main_b = 0x22000000u;
    constexpr std::uint32_t out_ptr = 0x20001000u;
    const std::uint32_t label255 = ppu_hle_inject_base + 0xFF0u;
    const std::uint32_t label254 = ppu_hle_inject_base + 0xFE0u;
    const std::uint32_t control = ppu_hle_inject_base + 0x2000u;
    const std::uint32_t control_get = control + 4;
    const std::uint32_t control_ref = control + 8;
    port_gcm_contract_begin(fifo);
    _putenv_s("RSX_CAP_OFFSCREEN", "1");
    if (rsx_d3d12_backend_init(1280, 720, "gcm-address-space") != 0) {
        std::fprintf(stderr, "[compat-check] GCM address-space D3D12 init FAIL\n");
        return 1;
    }

    auto* const out_address = reinterpret_cast<std::uint32_t*>(static_cast<std::uintptr_t>(out_ptr));
    vm_write32(out_ptr, 0);
    const int map_a = cellGcmMapMainMemory(main_a, 0x100000u, out_address);
    const std::uint32_t io_a = vm_read32(out_ptr);
    const int map_b = cellGcmMapMainMemory(main_b, 0x100000u, out_address);
    const std::uint32_t io_b = vm_read32(out_ptr);
    const int reverse_a = cellGcmIoOffsetToAddress(io_a + 0xFB0u, out_address);
    const bool io_roundtrip = reverse_a == 0 && vm_read32(out_ptr) == main_a + 0xFB0u;
    if (map_a != 0 || map_b != 0 || !io_roundtrip) {
        std::fprintf(stderr, "[compat-check] report destination mapping failed a=%d b=%d roundtrip=%d\n",
                     map_a, map_b, io_roundtrip);
        return 1;
    }

    constexpr unsigned report_bytes = 5 * 16;
    std::memset(vm_base + main_a + 0xFB0u, 0xCD, report_bytes);
    std::memset(vm_base + main_b + 0xFB0u, 0xA5, report_bytes);
    vm_write32(label255, 0x11223344u);
    vm_write32(label254, 0x55667788u);
    vm_write32(control, 0xAABBCCDDu);
    vm_write32(control_get, 0x10203040u);
    vm_write32(control_ref, 0x50607080u);

    std::uint32_t at = 0;
    method(fifo, at, 0, 0x1A8, 0x66626660u);
    for (unsigned i = 0; i < 5; ++i)
        method(fifo, at, 0, 0x1800, 0x01000000u | (i * 16u));
    auto transfer = [&](std::uint32_t output) {
        method(fifo, at, 1, 0x184, 0x66626660u);
        method(fifo, at, 1, 0x188, 0xFEED0001u);
        method(fifo, at, 1, 0x30C, 0);
        method(fifo, at, 1, 0x310, output + 0xFB0u);
        method(fifo, at, 1, 0x314, 80);
        method(fifo, at, 1, 0x318, 80);
        method(fifo, at, 1, 0x31C, 80);
        method(fifo, at, 1, 0x320, 1);
        method(fifo, at, 1, 0x324, 0x101);
        method(fifo, at, 1, 0x328, 0);
    };
    transfer(io_a);
    transfer(io_b);
    /* The report source is filled by the preceding real SET_REPORT methods. */
    vm_write32(control, at);
    cellGcm_rsx_process_fifo();

    const auto* first = vm_base + main_a + 0xFB0u;
    const bool bytes = std::memcmp(vm_base + main_b + 0xFB0u, first, report_bytes) == 0;
    bool ordered = true;
    bool padding = true;
    std::uint64_t previous_timestamp = 0;
    for (unsigned record = 0; record < 5; ++record) {
        const unsigned base = record * 16;
        std::uint64_t timestamp = 0;
        for (unsigned byte = 0; byte < 8; ++byte)
            timestamp = (timestamp << 8) | first[base + byte];
        ordered = ordered && timestamp != 0 && (record == 0 || timestamp >= previous_timestamp);
        previous_timestamp = timestamp;
        // No geometry was submitted. Both the sample count and the padding are zero.
        for (unsigned byte = 8; byte < 16; ++byte)
            padding &= first[base + byte] == 0;
    }
    const bool guards = vm_read32(label255) == 0x11223344u &&
                        vm_read32(label254) == 0x55667788u &&
                        vm_read32(control) == at && vm_read32(control_get) == at &&
                        vm_read32(control_ref) == 0x50607080u;
    const bool table = port_gcm_io_ea(io_a + 0xFB0u) == main_a + 0xFB0u &&
                       port_gcm_io_ea(io_b + 0xFB0u) == main_b + 0xFB0u;
    const bool reserved = cellGcmMapMainMemory(ppu_hle_inject_base, 0x100000u, out_address) < 0 &&
                          cellGcmMapEaIoAddress(ppu_hle_inject_base - 0x100000u, 0x800000u, 0x200000u) < 0 &&
                          cellGcmMapEaIoAddress(0xFFF00000u, 0x800000u, 0x200000u) < 0;
    const bool ok = bytes && ordered && padding && guards && table && reserved;
    std::fprintf(stderr, "[compat-check] SET_REPORT -> NV0039 bytes=%u timestamps=%d samples-padding=%d "
                 "labels-control=%d table=%d reserved=%d label255=%08X report-word=%08X %s\n",
                 bytes ? report_bytes : 0, ordered, padding, guards, table, reserved,
                 vm_read32(label255), vm_read32(main_a + 0xFF0u), ok ? "PASS" : "FAIL");
    cellGcmUnmapEaIoAddress(main_a);
    cellGcmUnmapEaIoAddress(main_b);
    return ok ? 0 : 1;
}
