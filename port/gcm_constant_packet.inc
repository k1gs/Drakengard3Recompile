/* NV4097 transform-constant uploads in a published FIFO packet. */
static int port_constant_header(u32 header)
{
    const u32 count = (header >> 18) & 0x7ffu;
    return count >= 2 && count <= 33 &&
           (header & ~(0x7ffu << 18)) == NV4097_SET_TRANSFORM_CONSTANT_LOAD;
}

static u32 port_constant_word(const u8 *data)
{
    return ((u32)data[0] << 24) | ((u32)data[1] << 16) | ((u32)data[2] << 8) | data[3];
}

static void port_apply_constant_packet(rsx_state *state, const u8 *data, u32 count)
{
    state->transform_constant_load = port_constant_word(data);
    for (u32 index = 0; index < count - 1; ++index)
    {
        /* LOAD is a 32-bit RSX register, including unsigned wrap. */
        const u32 slot = state->transform_constant_load + index / 4;
        if (slot >= RSX_MAX_VERTEX_CONSTANTS)
            continue;
        const u32 bits = port_constant_word(data + 4 + index * 4);
        memcpy(&state->vertex_constants[slot][index % 4], &bits, sizeof(bits));
        if (!state->vertex_constants_dirty)
        {
            state->vertex_constants_lo = state->vertex_constants_hi = slot;
            state->vertex_constants_dirty = 1;
        }
        else
        {
            if (slot < state->vertex_constants_lo)
                state->vertex_constants_lo = slot;
            if (slot > state->vertex_constants_hi)
                state->vertex_constants_hi = slot;
        }
    }
}

static int port_constant_dispatch_enabled(void)
{
#ifdef VM_SAMPLE_READS
    return 0;
#else
    static int enabled = -1;
    if (enabled < 0)
    {
        /* These instruments inspect individual method dispatches or VM reads. */
        static const char *const observers[] = {"PPU_RWATCH",
                                                "YDKJ_RWATCH",
                                                "PPU_RVAL",
                                                "PPU_FORCE_READ_ADDR",
                                                "PPU_HOTMAP",
                                                "PPU_SPINBT",
                                                "PT",
                                                "GCM_VAL_WATCH",
                                                "GCM_SUBCH_GATE",
                                                "GCM_OBJDBG",
                                                "RSX_LIVE_FEED_DBG",
                                                "RSX_TRACE",
                                                "RSX_CMDDBG",
                                                "RSX_MTRACE",
                                                "LOAD_DBG",
                                                "TCONST_DBG",
                                                "TCONST_FIFO",
                                                "SEQ_DBG"};
        enabled = !rsx_live_draw_enabled() && !rsx_draw_engine_enabled();
        for (size_t i = 0; enabled && i < sizeof(observers) / sizeof(observers[0]); ++i)
            if (getenv(observers[i]))
                enabled = 0;
    }
    return enabled;
#endif
}

/* The walker has already fetched the header on this page through vm_read32
 * and acquired PUT. One host page also lies in one 1 MiB IO mapping. Restrict
 * direct reads to ordinary RAM, excluding service registers and SPU MMIO. */
static int port_constant_room(u32 io, u32 ea, u32 put, u32 bytes)
{
    if ((io & 3) || io >= put || bytes > put - io || bytes > VM_PAGE_SIZE - ea % VM_PAGE_SIZE ||
        (ppu_vm_size && (u64)ea + bytes > ppu_vm_size))
        return 0;
    return (ea >= 0x10000u && ea < 0x10000000u && bytes <= 0x10000000u - ea) ||
           (ea >= 0x40000000u && ea < 0x58000000u && bytes <= 0x58000000u - ea);
}

static int port_consume_constant_packet(rsx_state *state, u32 io, u32 ea, u32 put, u32 header)
{
    if (!port_constant_header(header) || !port_constant_dispatch_enabled())
        return 0;
    const u32 count = (header >> 18) & 0x7ffu;
    if (!port_constant_room(io, ea, put, 4 + count * 4))
        return 0;
    port_apply_constant_packet(state, vm_base + ea + 4, count);
    return 1;
}
