/* Cache decoded attributes in CPU memory for one indexed triangle list.
 * The D3D12 upload heap is write-combined. Never read it back to expand a
 * repeated index. Check the actual guest bytes even on an index hit. */
enum { PORT_VERTEX_CACHE_SLOTS = 32 };

typedef struct {
    u32 attribute;
    u32 components;
    u32 bytes;
} PortVertexField;

typedef struct {
    u32 index;
    u8 source[16][16];
    VPSlot decoded[16];
} PortDecodedVertex;

typedef struct {
    PortVertexField fields[16];
    unsigned field_count;
    u32 valid;
    PortDecodedVertex entries[PORT_VERTEX_CACHE_SLOTS];
} PortIndexedVertices;

static u32 port_vertex_input_bytes(u32 type, u32 components)
{
    switch (type) {
    case 1: /* S1 */
    case 3: /* SF */
    case 5: /* S32K */
        return components * 2;
    case 4: /* UB */
    case 7: /* UB256 */
        return components;
    case 6: /* CMP */
        return 4;
    default:
        /* Float32 and the existing diagnostic decoder for unknown types. */
        return components * 4;
    }
}

static void port_begin_indexed_vertices(PortIndexedVertices* cache, const rsx_state* state)
{
    cache->field_count = 0;
    cache->valid = 0;
    for (unsigned i = 0; i < 16; ++i) {
        const rsx_vertex_attrib* attr = &state->vertex_attribs[i];
        if (!attr->enabled) continue;
        PortVertexField* field = &cache->fields[cache->field_count++];
        field->attribute = i;
        field->components = port_vertex_components(attr);
        field->bytes = port_vertex_input_bytes(attr->type, field->components);
    }
}

static unsigned port_expand_vertex(PortIndexedVertices* cache, const rsx_state* state,
                                   u32 index, VPSlot out[16])
{
    unsigned slot = index % PORT_VERTEX_CACHE_SLOTS;
    u32 bit = 1u << slot;
    PortDecodedVertex* entry = &cache->entries[slot];
    int same_index = (cache->valid & bit) && entry->index == index;
    unsigned reused = same_index;

    if (!(cache->valid & bit)) {
        /* Disabled attributes and unsupplied components keep their immediate
         * register values for this draw. Format and registers are RSX state. */
        memcpy(entry->decoded, state->vertex_data4f, sizeof(entry->decoded));
        cache->valid |= bit;
    }
    entry->index = index;
    for (unsigned i = 0; i < cache->field_count; ++i) {
        const PortVertexField* field = &cache->fields[i];
        unsigned attr = field->attribute;
        const u8* src = port_vertex_source(state, attr, index);
        u8* saved = entry->source[attr];
        if (same_index && memcmp(saved, src, field->bytes) == 0) continue;
        memcpy(saved, src, field->bytes);
        port_decode_attrib(&state->vertex_attribs[attr], attr, field->components,
                           saved, entry->decoded[attr].v);
        reused = 0;
    }
    rsx_vtx_pos_dbg(state, entry->decoded[0].v, port_vertex_components(&state->vertex_attribs[0]));
    memcpy(out, entry->decoded, sizeof(entry->decoded));
    return reused;
}

static u32 port_expand_indexed_vertices(const rsx_state* state, u32 first, u32 count, VPSlot* out)
{
    PortIndexedVertices cache;
    port_begin_indexed_vertices(&cache, state);
    u32 reused = 0;
    for (u32 k = 0; k < count; ++k) {
        u32 index = read_guest_index(state, first + k);
        reused += port_expand_vertex(&cache, state, index, &out[k * 16]);
    }
    return reused;
}
