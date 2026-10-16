"""Expose all 16 RSX fragment texture units in the build-local D3D12 backend."""
import re


def prepare(text):
    def once(old, new, count=1):
        nonlocal text
        if text.count(old) != count:
            raise ValueError('Pinned texture-unit anchor changed: ' + old[:90])
        text = text.replace(old, new)
    once('#define FRAME_COUNT', '#define PORT_TEXTURE_UNITS 16\n#define FRAME_COUNT')
    once('} tex[4];', '} tex[PORT_TEXTURE_UNITS];')
    once('int tex_rt[4];', 'int tex_rt[PORT_TEXTURE_UNITS];')
    once('cur_texs[4];', 'cur_texs[PORT_TEXTURE_UNITS];')
    once('MAX_DRAWS * 4)', 'MAX_DRAWS * PORT_TEXTURE_UNITS)')
    once('srv_range.NumDescriptors = 4;', 'srv_range.NumDescriptors = PORT_TEXTURE_UNITS;')
    once('D3D12_STATIC_SAMPLER_DESC samp[4]', 'D3D12_STATIC_SAMPLER_DESC samp[PORT_TEXTURE_UNITS]')
    once('_s < 4;', '_s < PORT_TEXTURE_UNITS;')
    once('rd.NumStaticSamplers = 4;', 'rd.NumStaticSamplers = PORT_TEXTURE_UNITS;')
    once('DRAW_SRV_BASE + _d * 4 +', 'DRAW_SRV_BASE + _d * PORT_TEXTURE_UNITS +')
    once('DRAW_SRV_BASE + d * 4)', 'DRAW_SRV_BASE + d * PORT_TEXTURE_UNITS)')
    once('if (unit < 4 &&', 'if (unit < PORT_TEXTURE_UNITS &&')
    once('for (int u = 0; u < 4; u++) if (dr->tex[u].set',
         'for (int u = 0; u < PORT_TEXTURE_UNITS; u++) if (dr->tex[u].set')
    # Keep the legacy four texscale rows in b1: the modern buffered decompiler
    # samples its texture arrays directly, and FP constants start after b1[4].
    begin = text.index('static void vp_record_cb(')
    end = text.index('\n}\n', begin) + 3
    prefix, cb, suffix = text[:begin], text[begin:end], text[end:]
    loop = 'for (int _u = 0; _u < 4; _u++)'
    if cb.count(loop) != 1 or prefix.count(loop) + suffix.count(loop) != 8:
        raise ValueError('Pinned texture loops changed')
    text = prefix.replace(loop, loop.replace('< 4;', '< PORT_TEXTURE_UNITS;')) + cb + suffix.replace(loop, loop.replace('< 4;', '< PORT_TEXTURE_UNITS;'))
    # All three draw recording paths share one complete state snapshot.
    pattern = r'for \(int _u = 0; _u < PORT_TEXTURE_UNITS; _u\+\+\) \{\s*dr->tex\[_u\]\.off = s_d3d\.cur_texs\[_u\]\.off;.*?dr->tex_rt\[_u\]\s*= -1;\s*\}'
    text, n = re.subn(pattern, 'port_snapshot_textures(dr);', text, flags=re.S)
    if n != 3:
        raise ValueError(f'Expected three draw texture snapshots, got {n}')
    helper = r'''
static void port_snapshot_textures(D3D12DrawRecord* dr)
{
    for(unsigned u=0;u<PORT_TEXTURE_UNITS;u++) {
#define COPY_TEX(field) dr->tex[u].field=s_d3d.cur_texs[u].field
        COPY_TEX(off);COPY_TEX(raw);COPY_TEX(w);COPY_TEX(h);COPY_TEX(fmt);
        COPY_TEX(ctrl1);COPY_TEX(mips);COPY_TEX(cube);COPY_TEX(set);
#undef COPY_TEX
        dr->tex_rt[u]=-1;
    }
}
'''
    once('static int init_d3d12(', helper + '\nstatic int init_d3d12(')
    fixture = r'''
int port_check_texture_units(void)
{
    D3D12DrawRecord dr={0};int ok=1;
    for(unsigned u=0;u<PORT_TEXTURE_UNITS;u++) {
        rsx_texture_state tex={0};tex.offset=0x1000+u*0x1000;
        tex.image_rect=((16+u)<<16)|(32+u);tex.format=(1u<<16)|(0x85u<<8)|1u;
        tex.control1=0xAAE4;
        d3d12_bind_texture(NULL,u,&tex);
    }
    port_snapshot_textures(&dr);
    memset(s_d3d.cur_texs,0,sizeof(s_d3d.cur_texs));
    for(unsigned u=0;u<PORT_TEXTURE_UNITS;u++) {
        if(!dr.tex[u].set || dr.tex[u].raw!=0x1000+u*0x1000 ||
           dr.tex[u].w!=16+u || dr.tex[u].h!=32+u ||
           dr.tex[u].ctrl1!=0xAAE4 || dr.tex_rt[u]!=-1) ok=0;
    }
    fprintf(stderr,"[compat-check] texture units bound/snapshotted=%u unit15=%08X %s\n",
        PORT_TEXTURE_UNITS,dr.tex[15].raw,ok?"PASS":"FAIL");
    return ok?0:1;
}
'''
    once('static void d3d12_set_vertex_attribs(', fixture + '\nstatic void d3d12_set_vertex_attribs(')
    return text
