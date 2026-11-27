/* Cache decoded attributes in CPU memory for one indexed triangle list.
 * The D3D12 upload heap is write-combined. Never read it back to expand a
 * repeated index. Check the actual guest bytes even on an index hit. */
enum { PORT_VERTEX_CACHE_SLOTS = 32 };

typedef struct {
    u32 attribute;
    u32 components;
    u32 bytes;
    u32 snapshot_offset;
} PortVertexField;

typedef struct {
    u32 first_field, field_count;
    u32 bytes, stride, base_ea;
} PortVertexSpan;

typedef struct {
    u32 index;
    u8 source[16 * 16];
    VPSlot decoded[16];
} PortDecodedVertex;

typedef struct {
    PortVertexField fields[16];
    PortVertexSpan spans[16];
    unsigned span_count;
    unsigned first_individual, field_count;
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

static u32 port_vertex_layout_offset(const rsx_vertex_attrib* attr)
{
    /* The other inputs retain register order and resolve at every fetch. */
    return !(attr->offset & 0x80000000u) && attr->frequency <= 1 ? attr->offset : UINT32_MAX;
}

static void port_begin_indexed_vertices(PortIndexedVertices* cache, const rsx_state* state)
{
    unsigned field_count = 0;
    cache->span_count = 0;
    cache->valid = 0;
    for (unsigned i = 0; i < 16; ++i) {
        const rsx_vertex_attrib* attr = &state->vertex_attribs[i];
        if (!attr->enabled) continue;
        /* Attribute register order need not match the layout in memory. */
        u32 order = port_vertex_layout_offset(attr);
        unsigned pos = field_count++;
        while (pos && port_vertex_layout_offset(&state->vertex_attribs[cache->fields[pos - 1].attribute]) > order) {
            cache->fields[pos] = cache->fields[pos - 1];
            --pos;
        }
        PortVertexField* field = &cache->fields[pos];
        field->attribute = i;
        field->components = port_vertex_components(attr);
        field->bytes = port_vertex_input_bytes(attr->type, field->components);
        field->snapshot_offset = 0;
    }

    u32 snapshot_offset = 0;
    PortVertexSpan* span = NULL;
    u32 span_offset = 0;
    u32 snapshot_start = 0;
    cache->field_count = field_count;
    cache->first_individual = field_count;
    for (unsigned i = 0; i < field_count; ++i) {
        PortVertexField* field = &cache->fields[i];
        const rsx_vertex_attrib* attr = &state->vertex_attribs[field->attribute];
        if (port_vertex_layout_offset(attr) == UINT32_MAX) {
            cache->first_individual = i;
            break;
        }
        /* Only join overlapping or adjacent fields of the same LOCAL stream.
         * A gap can include inaccessible guest memory. */
        if (!span || attr->stride != span->stride || attr->offset - span_offset > span->bytes) {
            span = &cache->spans[cache->span_count++];
            span->first_field = i;
            span->field_count = 0;
            span->bytes = 0;
            span->stride = attr->stride;
            span->base_ea = cellGcmResolveLocated(1, attr->offset);
            span_offset = attr->offset;
            snapshot_start = snapshot_offset;
        }
        u32 relative = attr->offset - span_offset;
        field->snapshot_offset = snapshot_start + relative;
        if (relative + field->bytes > span->bytes) span->bytes = relative + field->bytes;
        snapshot_offset = snapshot_start + span->bytes;
        ++span->field_count;
    }
    /* MAIN and frequency-divided inputs retain individual source lookups. */
    for (unsigned i = cache->first_individual; i < field_count; ++i) {
        cache->fields[i].snapshot_offset = snapshot_offset;
        snapshot_offset += cache->fields[i].bytes;
    }
}

static void port_decode_vertex_field(const rsx_state* state, const PortVertexField* field,
                                     PortDecodedVertex* vertex)
{
    unsigned attr = field->attribute;
    port_decode_attrib(&state->vertex_attribs[attr], attr, field->components,
                       vertex->source + field->snapshot_offset, vertex->decoded[attr].v);
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
    for (unsigned i = 0; i < cache->span_count; ++i) {
        const PortVertexSpan* span = &cache->spans[i];
        const PortVertexField* first = &cache->fields[span->first_field];
        u32 ea = span->base_ea + index * span->stride;
        if (ea > UINT32_MAX - (span->bytes - 1)) {
            /* Each field's guest start address wraps separately at 32 bits. */
            for (unsigned k = 0; k < span->field_count; ++k) {
                const PortVertexField* field = &first[k];
                const u8* src = port_vertex_source(state, field->attribute, index);
                port_decode_attrib(&state->vertex_attribs[field->attribute], field->attribute,
                                   field->components, src, entry->decoded[field->attribute].v);
            }
            reused = 0;
            continue;
        }
        const u8* src = vm_base + ea;
        u8* saved = entry->source + first->snapshot_offset;
        if (same_index && !memcmp(saved, src, span->bytes)) continue;
        memcpy(saved, src, span->bytes);
        for (unsigned k = 0; k < span->field_count; ++k)
            port_decode_vertex_field(state, &first[k], entry);
        reused = 0;
    }
    for (unsigned i = cache->first_individual; i < cache->field_count; ++i) {
        const PortVertexField* field = &cache->fields[i];
        const u8* src = port_vertex_source(state, field->attribute, index);
        u8* saved = entry->source + field->snapshot_offset;
        if (same_index && !memcmp(saved, src, field->bytes)) continue;
        memcpy(saved, src, field->bytes);
        port_decode_vertex_field(state, field, entry);
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
