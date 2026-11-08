"""Preserve the independent guest scissor through deferred draw submission."""
from pathlib import Path


def prepare(text):
    def once(old, new):
        nonlocal text
        if text.count(old) != 1:
            raise ValueError('Pinned scissor anchor changed: ' + old[:90])
        text = text.replace(old, new)

    once('    u32 vp_x, vp_y, vp_w, vp_h;',
         '    u32 vp_x, vp_y, vp_w, vp_h;\n    u32 sc_x, sc_y, sc_w, sc_h;')
    helper = r'''
/* Record on the FIFO owner. A zero extent is an empty scissor, not an unset
 * viewport. Missing guest state alone defaults to the entire attachment. */
static void port_snapshot_scissor(D3D12DrawRecord* dr,const rsx_state* st)
{
    dr->sc_x=st?st->scissor_x:0;dr->sc_y=st?st->scissor_y:0;
    dr->sc_w=st?st->scissor_w:0xFFFFFFFFu;
    dr->sc_h=st?st->scissor_h:0xFFFFFFFFu;
}

static D3D12_RECT port_draw_scissor(const D3D12DrawRecord* dr,u32 width,u32 height)
{
    u64 right=(u64)dr->sc_x+dr->sc_w,bottom=(u64)dr->sc_y+dr->sc_h;
    D3D12_RECT r={(LONG)(dr->sc_x<width?dr->sc_x:width),
                  (LONG)(dr->sc_y<height?dr->sc_y:height),
                  (LONG)(right<width?right:width),
                  (LONG)(bottom<height?bottom:height)};
    return r;
}

'''
    check = (Path(__file__).resolve().parent.parent /
             'port/checks/scissor_gpu.inc').read_text(encoding='utf-8')
    once('static void port_snapshot_textures(',
         helper + check + '\nstatic void port_snapshot_textures(')
    once('static void port_snapshot_textures(D3D12DrawRecord* dr)\n{',
         'static void port_snapshot_textures(D3D12DrawRecord* dr)\n{\n'
         '    port_snapshot_scissor(dr,s_d3d.current_rsx_state);')
    once('                 * full target. Scissor tracks the same rect. */',
         '                 * full target. Scissor is an independent guest register. */')
    once('''                    D3D12_RECT dsc = {(LONG)dvp.TopLeftX, (LONG)dvp.TopLeftY,
                                      (LONG)(dvp.TopLeftX + dvp.Width),
                                      (LONG)(dvp.TopLeftY + dvp.Height)};''',
         '                    D3D12_RECT dsc=port_draw_scissor(dr,(u32)tw,(u32)th);')
    return text
