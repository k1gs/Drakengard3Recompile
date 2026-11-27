static int port_check_constant_stream_order(void)
{
    extern void port_gcm_contract_begin(u32);
    extern void cellGcm_rsx_process_fifo(void);
    const u32 fifo = 0x40000000u;
    const u32 header = (2u << 18) | NV4097_SET_TRANSFORM_CONSTANT_LOAD;
    const u32 crossing = VM_PAGE_SIZE - 4;
    const u32 end = crossing + 24;
    port_gcm_contract_begin(fifo);
    vm_write32(fifo, header);
    vm_write32(fifo + 4, 7);
    vm_write32(fifo + 8, 0x3f800000u);
    vm_write32(fifo + 12, 0x20000000u | crossing);
    for (u32 i = 0; i < 2; ++i)
    {
        u32 ea = fifo + crossing + 12 * i;
        vm_write32(ea, header);
        vm_write32(ea + 4, 8 + i);
        vm_write32(ea + 8, i ? 0x40400000u : 0x40000000u);
    }

    vm_write32(GCM_CONTROL_GUEST_ADDR, 12);
    cellGcm_rsx_process_fifo();
    const rsx_state *state = (const rsx_state *)s_port_gcm_render_state;
    if (!state || s_fifo_getoff != 12 || vm_read32(GCM_CONTROL_GUEST_ADDR + 4) != 12 ||
        state->vertex_constants[7][0] != 1.0f || state->vertex_constants[8][0] != 0)
        return 0;

    vm_write32(fifo + end - 4, 0x40800000u);
    vm_write32(GCM_CONTROL_GUEST_ADDR, end);
    cellGcm_rsx_process_fifo();
    return s_fifo_getoff == end && vm_read32(GCM_CONTROL_GUEST_ADDR + 4) == end &&
           state->vertex_constants[7][0] == 1.0f && state->vertex_constants[8][0] == 2.0f &&
           state->vertex_constants[9][0] == 4.0f &&
           !port_constant_room(crossing, fifo + crossing, end, 12) &&
           !port_constant_room(0, fifo, 8, 12) &&
           !port_constant_room(0, GCM_CONTROL_GUEST_ADDR, 12, 12) &&
           !port_constant_room(0, 0xe0004000u, 12, 12);
}

static int port_check_constant_lanes(u32 load, u32 count, int initially_dirty)
{
    u32 values[33] = {load};
    u8 bytes[33 * 4];
    for (u32 i = 1; i < count; ++i)
        values[i] = 0x3f000000u + i * 0x01010101u;
    values[1] = 0x80000000u; /* signed zero */
    values[2] = 0x7fc00001u; /* NaN with payload */
    for (u32 i = 0; i < count; ++i)
        for (u32 lane = 0; lane < 4; ++lane)
            bytes[4 * i + lane] = (u8)(values[i] >> (24 - 8 * lane));

    rsx_state expected = {0};
    expected.vertex_constants_dirty = initially_dirty;
    expected.vertex_constants_lo = initially_dirty ? 400 : 0;
    expected.vertex_constants_hi = initially_dirty ? 401 : 0;
    rsx_state actual;
    memcpy(&actual, &expected, sizeof(expected));
    port_apply_constant_packet(&actual, bytes, count);
    rsx_process_method(&expected, NV4097_SET_TRANSFORM_CONSTANT_LOAD, values[0]);
    for (u32 i = 1; i < count; ++i)
        rsx_process_method(&expected, NV4097_SET_TRANSFORM_CONSTANT + (i - 1) * 4, values[i]);
    return memcmp(&expected, &actual, sizeof(expected)) == 0;
}

int port_check_gcm_constant_packet(void)
{
    const u32 loads[] = {0, 11, RSX_MAX_VERTEX_CONSTANTS - 1, RSX_MAX_VERTEX_CONSTANTS,
                         0xfffffffeu};
    int lanes = 1;
    for (size_t i = 0; i < sizeof(loads) / sizeof(loads[0]); ++i)
        for (u32 count = 2; count <= 33; ++count)
            for (int dirty = 0; dirty < 2; ++dirty)
                lanes &= port_check_constant_lanes(loads[i], count, dirty);
    const u32 good = (2u << 18) | NV4097_SET_TRANSFORM_CONSTANT_LOAD;
    const u32 invalid[] = {good | 1u,
                           good | (2u << 29),
                           good | (1u << 13),
                           good | (1u << 16),
                           (1u << 18) | NV4097_SET_TRANSFORM_CONSTANT_LOAD,
                           (34u << 18) | NV4097_SET_TRANSFORM_CONSTANT_LOAD,
                           good + 4};
    int headers = port_constant_header(good);
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i)
        headers &= !port_constant_header(invalid[i]);
    int ordered = port_check_constant_stream_order();
    int ok = headers && lanes && ordered;
    fprintf(stderr,
            "[compat-check] FIFO constants headers=%d registers=%d publication-and-jump=%d %s\n",
            headers, lanes, ordered, ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
