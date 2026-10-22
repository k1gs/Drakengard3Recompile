"""Checked build-local CMP vertex decoding for the D3D12 renderer."""


def prepare(sdk):
    source = (sdk / 'libs/video/rsx_vertex_fetch.c').read_text(encoding='utf-8')
    anchor = 'void rsx_fetch_attrib('
    if source.count(anchor) != 1 or 'case 6:' in source:
        raise ValueError('Pinned vertex fetch implementation changed')
    fetch = source[source.index(anchor):].replace(anchor, 'static void port_fetch_attrib(', 1)
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
'''
