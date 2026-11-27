/* Exercise the same indexed upload loop as a triangle-list draw and compare
 * every slot, including disabled attributes, to the scalar fetch path. */
static int port_check_index_stream(rsx_state* state, int short_indices)
{
    const u32 collision = short_indices ? 35 : 0x10003;
    const u32 indices[] = {9, 3, 4, 3, collision, 3, 4, 3, 5, 3};
    enum { count = sizeof(indices) / sizeof(indices[0]) - 1 };
    state->index_array_offset = 0x8000;
    state->index_array_dma = short_indices ? 0x10 : 0;
    u8* data = vm_base + cellGcmResolveLocated(1, state->index_array_offset);

    for (unsigned i = 0; i <= count; ++i) {
        u32 index = indices[i];
        unsigned width = short_indices ? 2 : 4;
        for (unsigned byte = 0; byte < width; ++byte)
            data[i * width + byte] = (u8)(index >> (8 * (width - byte - 1)));
        for (unsigned attr = 0; attr < 8; ++attr) {
            u8* src = (u8*)port_vertex_source(state, attr, index);
            for (unsigned k = 0; k < 16; ++k)
                src[k] = (u8)(0x31 + attr + k + index + (index >> 16));
        }
    }

    VPSlot actual[count * 16];
    u32 reused = port_expand_indexed_vertices(state, 1, count, actual);
    for (unsigned i = 0; i < count; ++i) {
        VPSlot expected[16];
        read_vp_vertex(state, indices[i + 1], expected);
        if (memcmp(expected, &actual[i * 16], sizeof(expected))) return 0;
    }
    /* 3 and collision replace each other; all 32 index bits participate. */
    return reused == 4;
}

static int port_check_interleaved_vertices(rsx_state* state)
{
    const u32 base = 0x1800000;
    const u32 offsets[] = {8, 0, 4, 16, 4};
    const u32 index = 3;
    memset(state->vertex_attribs, 0, sizeof(state->vertex_attribs));
    for (unsigned i = 0; i < 5; ++i) {
        rsx_vertex_attrib* attr = &state->vertex_attribs[i];
        attr->enabled = 1;
        attr->offset = base + offsets[i];
        attr->stride = 24;
        attr->type = i == 4 ? 5 : 7;
        attr->size = i == 4 ? 2 : 4;
    }
    u8* record = vm_base + cellGcmResolveLocated(1, base + index * 24);
    for (unsigned i = 0; i < 24; ++i) record[i] = (u8)(i + 1);
    PortIndexedVertices cache;
    VPSlot before[16], after[16], expected[16];
    port_begin_indexed_vertices(&cache, state);
    if (cache.span_count != 2 || cache.spans[0].bytes != 12 || cache.spans[1].bytes != 4) return 0;
    port_expand_vertex(&cache, state, index, before);
    read_vp_vertex(state, index, expected);
    if (memcmp(before, expected, sizeof(before))) return 0;
    record[13] ^= 0xff; /* No enabled attribute covers this byte. */
    if (!port_expand_vertex(&cache, state, index, after)) return 0;
    record[5] ^= 0xff;  /* Attributes 2 and 4 interpret these bytes differently. */
    if (port_expand_vertex(&cache, state, index, after)) return 0;
    read_vp_vertex(state, index, expected);
    return memcmp(after, expected, sizeof(after)) == 0 &&
           memcmp(&before[2], &after[2], sizeof(VPSlot)) != 0 &&
           memcmp(&before[4], &after[4], sizeof(VPSlot)) != 0;
}

static int port_check_vertex_remap(rsx_state* state)
{
    const u32 io = 0x03000000, page = 0x100000;
    const u32 ea_a = 0x22000000, ea_b = 0x24000000, ea_c = 0x26000000;
    if (cellGcmMapEaIoAddress(ea_a, io, page)) return 0;
    if (cellGcmMapEaIoAddress(ea_b, io + page, page)) {
        cellGcmUnmapIoAddress(io);
        return 0;
    }
    memset(state->vertex_attribs, 0, sizeof(state->vertex_attribs));
    for (unsigned i = 0; i < 2; ++i) {
        rsx_vertex_attrib* attr = &state->vertex_attribs[i];
        attr->enabled = 1;
        attr->offset = 0x80000000u | (io + page - 4 + i * 4);
        attr->stride = 8;
        attr->type = 7;
        attr->size = 4;
    }
    state->vertex_attribs[2] = state->vertex_attribs[0];
    state->vertex_attribs[2].offset = 0x8000;
    memset(vm_base + cellGcmResolveLocated(1, 0x8000), 0x44, 4);
    memset(vm_base + ea_a + page - 4, 0x11, 4);
    memset(vm_base + ea_b, 0x22, 4);
    memset(vm_base + ea_c + page - 4, 0x33, 4);
    PortIndexedVertices cache;
    VPSlot out[16];
    port_begin_indexed_vertices(&cache, state);
    port_expand_vertex(&cache, state, 0, out);
    int ok = out[0].v[0] == 0x11 && out[1].v[0] == 0x22 && out[2].v[0] == 0x44;
    if (cellGcmUnmapIoAddress(io) || cellGcmMapEaIoAddress(ea_c, io, page)) ok = 0;
    else {
        ok = !port_expand_vertex(&cache, state, 0, out) && ok;
        ok = out[0].v[0] == 0x33 && out[1].v[0] == 0x22 && out[2].v[0] == 0x44 && ok;
        if (cellGcmUnmapIoAddress(io)) ok = 0;
    }
    if (cellGcmUnmapIoAddress(io + page)) ok = 0;
    return ok;
}

static int port_check_local_vertex_wrap(rsx_state* state)
{
    const u32 last_word = UINT32_MAX - 3;
    const u32 index = last_word - cellGcmResolveLocated(1, 0);
    memset(state->vertex_attribs, 0, sizeof(state->vertex_attribs));
    for (unsigned i = 0; i < 2; ++i) {
        rsx_vertex_attrib* attr = &state->vertex_attribs[i];
        attr->enabled = 1;
        attr->offset = 4 * i;
        attr->stride = 1;
        attr->type = 7;
        attr->size = 4;
    }
    memset(vm_base + last_word, 0x5a, 4);
    memset(vm_base, 0xa5, 4);
    PortIndexedVertices cache;
    VPSlot out[16];
    port_begin_indexed_vertices(&cache, state);
    port_expand_vertex(&cache, state, index, out);
    if (out[0].v[3] != 0x5a || out[1].v[0] != 0xa5) return 0;
    vm_base[0] = 0x42;
    port_expand_vertex(&cache, state, index, out);
    return out[0].v[3] == 0x5a && out[1].v[0] == 0x42;
}

int port_check_indexed_vertices(void)
{
    rsx_state* state = (rsx_state*)calloc(1, sizeof(*state));
    if (!state) return 1;
    for (unsigned attr = 0; attr < 16; ++attr) {
        for (unsigned lane = 0; lane < 4; ++lane)
            state->vertex_data4f[attr][lane] = 10.0f * attr + lane + 0.25f;
    }
    const unsigned types[] = {2, 3, 4, 1, 5, 6, 7, 2};
    const unsigned sizes[] = {3, 2, 4, 1, 0, 1, 3, 4};
    for (unsigned i = 0; i < 8; ++i) {
        rsx_vertex_attrib* attr = &state->vertex_attribs[i];
        attr->enabled = 1;
        attr->type = types[i];
        attr->size = sizes[i];
        attr->stride = i == 7 ? 0 : 16;
        attr->offset = (i + 1) * 0x200000;
    }
    state->vertex_attribs[2].frequency = 2;
    state->vertex_attribs[3].frequency = 3;
    state->frequency_divider_op = 1u << 2;
    int short_ok = port_check_index_stream(state, 1);
    int wide_ok = port_check_index_stream(state, 0);

    PortIndexedVertices cache;
    VPSlot before[16], after[16], expected[16];
    port_begin_indexed_vertices(&cache, state);
    port_expand_vertex(&cache, state, 3, before);
    u8* source = (u8*)port_vertex_source(state, 2, 3);
    source[1] ^= 0xff;
    int changed = !port_expand_vertex(&cache, state, 3, after);
    read_vp_vertex(state, 3, expected);
    changed = changed && memcmp(before, after, sizeof(after)) != 0
                      && memcmp(expected, after, sizeof(after)) == 0;

    state->vertex_attribs[0].enabled = 0;
    state->vertex_data4f[0][0] = -10.5f;
    port_begin_indexed_vertices(&cache, state);
    int fresh = !port_expand_vertex(&cache, state, 3, after);
    read_vp_vertex(state, 3, expected);
    fresh = fresh && memcmp(expected, after, sizeof(after)) == 0;
    int interleaved = port_check_interleaved_vertices(state);
    int remap = port_check_vertex_remap(state);
    int wrap = port_check_local_vertex_wrap(state);
    free(state);

    int ok = short_ok && wide_ok && changed && fresh && interleaved && remap && wrap;
    fprintf(stderr, "[compat-check] indexed vertices: BE16=%d BE32=%d in-draw write=%d "
                    "new draw=%d interleaved=%d MAIN remap=%d LOCAL wrap=%d %s\n",
            short_ok, wide_ok, changed, fresh, interleaved, remap, wrap, ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
