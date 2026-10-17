#include "ppu_recomp.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <climits>

extern "C" void port_vpkswss(ppu_context *ctx, unsigned dst, unsigned left, unsigned right)
{
    // Lifted vectors store architectural lanes as big-endian bytes. Read all
    // sources before storing: audio conversion uses dst == left.
    uint8_t result[16];
    bool saturated = false;
    for (unsigned i = 0; i < 8; i++)
    {
        const uint8_t *p = ctx->vr[i < 4 ? left : right].b + (i % 4) * 4;
        uint32_t bits =
            (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
        int32_t value;
        std::memcpy(&value, &bits, 4);
        if (value > INT16_MAX)
        {
            value = INT16_MAX;
            saturated = true;
        }
        else if (value < INT16_MIN)
        {
            value = INT16_MIN;
            saturated = true;
        }
        uint16_t packed = static_cast<uint16_t>(value);
        result[2 * i] = static_cast<uint8_t>(packed >> 8);
        result[2 * i + 1] = static_cast<uint8_t>(packed);
    }
    std::memcpy(ctx->vr[dst].b, result, 16);
    if (saturated)
        ctx->vscr |= 1u; // VSCR.SAT is sticky; preserve all other bits.
}
