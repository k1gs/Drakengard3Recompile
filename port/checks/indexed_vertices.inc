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
    free(state);

    int ok = short_ok && wide_ok && changed && fresh;
    fprintf(stderr, "[compat-check] indexed vertices: BE16=%d BE32=%d in-draw write=%d "
                    "new draw=%d %s\n", short_ok, wide_ok, changed, fresh, ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
