"""Preserve colour DMA addresses and linear initialization pitch per command.

This does not implement format/size/pitch reinterpretation of GPU-written
aliases. Existing same-address native-layout reuse remains a separate contract.
"""
from pathlib import Path


def prepare(text):
    def once(old, new, count=1):
        nonlocal text
        if text.count(old) != count:
            raise ValueError('Pinned colour-surface anchor changed: ' + old[:110])
        text = text.replace(old, new)

    root = Path(__file__).resolve().parents[1]
    once('    u32 rt_fmt;', '    struct { u32 address, pitch; } color[4];\n    u32 rt_fmt;')
    once('    u32                   dxgi;',
         '    u32                   address, source_pitch;\n    u32                   dxgi;')
    helper = (root / 'port/renderer_color_surfaces.inc').read_text(encoding='utf-8')
    once('static void port_snapshot_textures(', helper + '\nstatic void port_snapshot_textures(')
    once('static void port_snapshot_textures(D3D12DrawRecord* dr)\n{',
         'static void port_snapshot_textures(D3D12DrawRecord* dr)\n{\n    port_snapshot_color(dr,s_d3d.current_rsx_state);')
    once('    r->rt_fmt=s_d3d.current_rsx_state?s_d3d.current_rsx_state->surface_format:0;',
         '    r->rt_fmt=s_d3d.current_rsx_state?s_d3d.current_rsx_state->surface_format:0;\n'
         '    port_snapshot_color(r,s_d3d.current_rsx_state);')
    start = text.index('/* Which offscreen surface (if any) does the current RSX state render to?')
    end = text.index('static u32 current_rt_off(', start)
    text = text[:start] + '''/* Display offsets name local-memory buffers only. Raw offsets remain for
 * display routing/diagnostics; resource lookup uses captured effective EAs. */
''' + text[end:]
    once('    if (cellGcmOffsetIsDisplay(raw)) return 0;',
         '    if (port_color_dma[sel]==0xFEED0000u && cellGcmOffsetIsDisplay(raw)) return 0;')
    start = text.index('static int off_rt_find(u32 off)')
    end = text.index('\n}\n', start) + 3
    text = text[:start] + '''static int off_rt_find(u32 address)
{
    for(int i=0;i<MAX_OFF_RTS;i++)
        if(s_d3d.off_rt[i].res && s_d3d.off_rt[i].address==address)return i;
    return -1;
}
''' + text[end:]
    once('static int off_rt_get(u32 off, u32 w, u32 h, u32 rsx_fmt)',
         'static int off_rt_get(u32 off, u32 address, u32 source_pitch, u32 w, u32 h, u32 rsx_fmt)')
    once('    int slot = off_rt_find(off);', '    int slot = off_rt_find(address);')
    once('    r->off = off; r->w = w; r->h = h; r->dxgi = (u32)want_fmt;',
         '    r->off = off; r->address=address;r->source_pitch=source_pitch;\n'
         '    r->w = w; r->h = h; r->dxgi = (u32)want_fmt;')
    once('''        extern u32 cellGcmResolveOffset(u32);
        u32 bpp =''', '        u32 bpp =')
    once('        u32 src = cellGcmResolveOffset(off);', '''        u32 src=address;
        u32 source_stride=source_pitch?source_pitch:w*bpp;
        if(source_stride<w*bpp || (u64)src+(u64)(h-1)*source_stride+(u64)w*bpp>0x100000000ull) {
            fprintf(stderr,"[color-surface] invalid linear source address=%08X pitch=%u %ux%u bpp=%u\\n",
                src,source_stride,w,h,bpp);abort();
        }''')
    once('                        const u8* srow = sp + (u64)y * w * bpp;',
         '                        const u8* srow = sp + (u64)y * source_stride;')
    once('off_rt_get(dr->rt_off, dr->rt_w, dr->rt_h, dr->rt_fmt)',
         'off_rt_get(dr->rt_off, dr->color[0].address, dr->color[0].pitch, dr->rt_w, dr->rt_h, dr->rt_fmt)')
    once('off_rt_get(dr->rt_mrt[_m], dr->rt_w, dr->rt_h, dr->rt_fmt)',
         'off_rt_get(dr->rt_mrt[_m], dr->color[_m+1].address, dr->color[_m+1].pitch, dr->rt_w, dr->rt_h, dr->rt_fmt)')
    once('off_rt_find(dr->rt_off)', 'off_rt_find(dr->color[0].address)')
    once('off_rt_find(dr->rt_mrt[_m])', 'off_rt_find(dr->color[_m+1].address)')
    once('off_rt_find(dr->tex[_u].raw)', 'off_rt_find(dr->tex[_u].off)')
    # These entry points explicitly request local/display offsets, not textures.
    once('off_rt_find(want)', 'off_rt_find(port_color_address(0xFEED0000u,want))')
    once('off_rt_find((u32)strtoul(sv, NULL, 16))',
         'off_rt_find(port_color_address(0xFEED0000u,(u32)strtoul(sv, NULL, 16)))', 2)
    # Add the actual address and retained initialization stride to capture data.
    once('"offrt[%d] off=0x%08X %ux%u dxgi=%u\\n", i, s_d3d.off_rt[i].off,\n'
         '                        s_d3d.off_rt[i].w, s_d3d.off_rt[i].h, s_d3d.off_rt[i].dxgi)',
         '"offrt[%d] off=0x%08X address=0x%08X pitch=%u %ux%u dxgi=%u\\n", i, s_d3d.off_rt[i].off,\n'
         '                        s_d3d.off_rt[i].address,s_d3d.off_rt[i].source_pitch,\n'
         '                        s_d3d.off_rt[i].w, s_d3d.off_rt[i].h, s_d3d.off_rt[i].dxgi)')
    text += '\n' + (root / 'port/checks/color_surfaces_gpu.inc').read_text(encoding='utf-8')
    return text


def commands(text):
    entry='int rsx_process_method(rsx_state* state, u32 method, u32 data)\n{'
    init='void rsx_state_init(rsx_state* state)\n{'
    if text.count(entry)!=1 or text.count(init)!=1:
        raise ValueError('Pinned colour DMA command entry changed')
    text=text.replace(entry,entry+'\n    if(port_renderer_color_method(method,data)) { state->surface_dirty=1;return 0; }')
    text=text.replace(init,init+'\n    port_renderer_color_reset();')
    return 'void port_renderer_color_reset(void);\nint port_renderer_color_method(unsigned int,unsigned int);\n'+text
