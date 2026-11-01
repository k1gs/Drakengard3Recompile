"""Checked build-local vertex decoding for the D3D12 renderer."""


def prepare(sdk):
    source = (sdk / 'libs/video/rsx_vertex_fetch.c').read_text(encoding='utf-8')
    anchor = 'void rsx_fetch_attrib('
    if source.count(anchor) != 1 or 'case 6:' in source:
        raise ValueError('Pinned vertex fetch implementation changed')
    fetch = source[source.index(anchor):].replace(anchor, 'static void port_fetch_attrib(', 1)
    # An enabled zero-stride array broadcasts the element at its memory offset.
    # Treating it as disabled substitutes the immediate attribute register.
    guard = '    if (!a->enabled || a->stride == 0 || !vm_base) return;'
    if fetch.count(guard) != 1:
        raise ValueError('Pinned vertex array enable guard changed')
    fetch = fetch.replace(guard, '    if (!a->enabled || !vm_base) return;')
    anchor = '    case 7: /* UB256: u8 unnormalized */'
    if fetch.count(anchor) != 1:
        raise ValueError('Pinned vertex format switch changed')
    fetch = fetch.replace(anchor, '''    case 6: /* CMP: one BE word supplies all three components, even size=1. */
        port_decode_cmp_vertex(p, out);
        break;
''' + anchor)
    return r'''
/* CMP expands signed 11/11/10 fields into the high bits of signed 16-bit
 * lanes, then normalizes by 32767. Do not divide by 1023/511 or clamp.
 * Semantics oracle (read only): RPCS3 RSXVertexFetch.glsl, COMP32 branch. */
static void port_decode_cmp_vertex(const u8* p,float out[4])
{
    u32 word=((u32)p[0]<<24)|((u32)p[1]<<16)|((u32)p[2]<<8)|p[3];
    for(unsigned i=0;i<3;i++) {
        unsigned shift=i==0?0:i==1?11:22;
        unsigned bits=i==2?10:11;
        u32 raw=(word>>shift)&((1u<<bits)-1);
        int value=(int)raw;
        if(raw&(1u<<(bits-1))) value-=(int)(1u<<bits);
        out[i]=(float)(value*(1<<(16-bits)))/32767.0f;
    }
    out[3]=1.0f;
}
''' + fetch + r'''
int port_check_compressed_vertices(void)
{
    unsigned checked=0;int ok=1;
    for(unsigned lane=0;lane<3;lane++) {
        unsigned bits=lane==2?10:11,shift=lane==0?0:lane==1?11:22;
        for(unsigned raw=0;raw<(1u<<bits);raw++) {
            u32 word=raw<<shift;
            u8 bytes[4]={(u8)(word>>24),(u8)(word>>16),(u8)(word>>8),(u8)word};
            float out[4];port_decode_cmp_vertex(bytes,out);
            int value=raw<(1u<<(bits-1))?(int)raw:(int)raw-(int)(1u<<bits);
            float expected=(float)((double)value*(lane==2?64.0:32.0)/32767.0);
            if(fabsf(out[lane]-expected)>0.0000001f || out[3]!=1.0f) ok=0;
            for(unsigned other=0;other<3;other++) if(other!=lane && out[other]!=0.0f) ok=0;
            checked++;
        }
    }
    fprintf(stderr,"[compat-check] CMP vertex BE lanes: %u values %s\n",checked,ok?"PASS":"FAIL");
    return ok?0:1;
}

/* Enabled zero-stride arrays still source memory: every vertex reads element
 * zero. Only a disabled array uses the immediate attribute register. */
int port_check_zero_stride_vertices(void)
{
    extern void vm_write32(uint64_t,uint32_t);
    rsx_state* st=(rsx_state*)calloc(1,sizeof(*st));
    if(!st) return 2;
    st->vertex_data4f[4][0]=0.25f;st->vertex_data4f[4][1]=0.5f;
    st->vertex_data4f[4][2]=0.75f;st->vertex_data4f[4][3]=1.0f;
    rsx_vertex_attrib* a=&st->vertex_attribs[4];
    a->enabled=1;a->size=4;a->type=4;a->stride=0;a->format=0x44;a->offset=0x2000;
    u32 ea=cellGcmResolveLocated(1,a->offset);
    vm_write32(ea,0xFFFFFFFFu);vm_write32(ea+4,0x204080FFu);
    float out[4];int repeated=1,changed=1,disabled=1,stepped=1,partial=1,floats=1;
    const u32 indices[]={0,1,715,65535};
    for(unsigned n=0;n<4;n++) {
        port_fetch_attrib(st,4,indices[n],out);
        for(unsigned k=0;k<4;k++) if(out[k]!=1.0f) repeated=0;
    }
    vm_write32(ea,0x10203040u);
    a->frequency=7;st->frequency_divider_op=1u<<4;
    port_fetch_attrib(st,4,715,out);
    for(unsigned k=0;k<4;k++) if(fabsf(out[k]-(float)((k+1)*16)/255.0f)>1e-7f) changed=0;
    a->enabled=0;
    port_fetch_attrib(st,4,715,out);
    if(memcmp(out,st->vertex_data4f[4],sizeof(out))) disabled=0;
    a->enabled=1;a->frequency=1;a->stride=4;
    port_fetch_attrib(st,4,1,out);
    const float expected[]={32/255.0f,64/255.0f,128/255.0f,1};
    for(unsigned k=0;k<4;k++) if(fabsf(out[k]-expected[k])>1e-7f) stepped=0;
    a->stride=0;a->size=2;
    port_fetch_attrib(st,4,123,out);
    if(fabsf(out[0]-16/255.0f)>1e-7f || fabsf(out[1]-32/255.0f)>1e-7f ||
       out[2]!=0.75f || out[3]!=1) partial=0;
    a->type=2;a->size=4;
    vm_write32(ea,0x3FC00000u);vm_write32(ea+4,0xC0000000u);
    vm_write32(ea+8,0x00000000u);vm_write32(ea+12,0x3F800000u);
    port_fetch_attrib(st,4,65535,out);
    if(out[0]!=1.5f || out[1]!=-2 || out[2]!=0 || out[3]!=1) floats=0;
    int ok=repeated&&changed&&disabled&&stepped&&partial&&floats;
    fprintf(stderr,"[compat-check] zero-stride vertices: repeated=%d memory-update=%d disabled=%d stepped=%d partial=%d float=%d %s\n",
            repeated,changed,disabled,stepped,partial,floats,ok?"PASS":"FAIL");
    free(st);return ok?0:1;
}
'''
