"""Prepare texture resources only for the exact selected fragment PSO."""
from pathlib import Path


def prepare(text):
    def once(old, new):
        nonlocal text
        if text.count(old) != 1:
            raise ValueError('Pinned texture prepass anchor changed: '+old[:90])
        text=text.replace(old,new)
    begin=text.index('        double _sv0 = perf_on() ? perf_now() : 0.0;')
    end=text.index('        if (perf_on()) s_perf_srv += perf_now() - _sv0;',begin)
    block=text[begin:end]
    prefix='        double _sv0 = perf_on() ? perf_now() : 0.0;\n        for (int _u = 0; _u < PORT_TEXTURE_UNITS; _u++) {\n'
    if not block.startswith(prefix) or not block.endswith('        }\n'):
        raise ValueError('Pinned texture prepass loop changed')
    body=block[len(prefix):-len('        }\n')]
    if body.count('continue;')!=3:
        raise ValueError('Pinned texture resolution exits changed')
    body=body.replace('continue;','return;')
    once(block,'        double _sv0 = perf_on() ? perf_now() : 0.0;\n'
         '        port_prepare_texture_slots(dr,_d,texture_mask,port_resolve_texture_unit,port_null_texture_unit);\n')
    start=text.index('        double _ps0 = perf_on() ? perf_now() : 0.0;')
    end=text.index('        if (perf_on()) s_perf_pso += perf_now() - _ps0;',start)
    end+=len('        if (perf_on()) s_perf_pso += perf_now() - _ps0;\n')
    pso=text[start:end]
    if pso.count('if (dr->fp_addr) vp_get_fp_pso(')!=1:
        raise ValueError('Pinned exact PSO lookup changed')
    text=text[:start]+text[end:]
    pso=pso.replace('        if (dr->fp_addr) vp_get_fp_pso(',
        '        ID3D12PipelineState* texture_pso=NULL;\n        if (dr->fp_addr) texture_pso=vp_get_fp_pso(')
    pso+='        u32 texture_mask=port_texture_prepass_mask(texture_pso);\n'
    once('        double _sv0 = perf_on() ? perf_now() : 0.0;',pso+'        double _sv0 = perf_on() ? perf_now() : 0.0;')
    helper=r'''
/* Read-only diagnostic snapshots are not transactional. FIFO ownership
 * serializes preparation; these counters do not control rendering. */
uint64_t port_texture_prepass_draws,port_texture_prepared_units,port_texture_skipped_units;
static u32 port_texture_prepass_mask(ID3D12PipelineState* selected)
{
    /* The lookup just selected this draw's exact cache entry. A missing FP
     * falls back to legacy shaders, so retain every slot in that case. */
    return selected?port_active_fp_samples:((1u<<PORT_TEXTURE_UNITS)-1u);
}
typedef void (*PortPrepareTextureUnit)(D3D12DrawRecord*,u32,int);
static void port_prepare_texture_slots(D3D12DrawRecord* dr,u32 draw,u32 mask,
                                      PortPrepareTextureUnit resolve,PortPrepareTextureUnit null_view)
{
    port_texture_prepass_draws++;
    for(int u=0;u<PORT_TEXTURE_UNITS;u++) {
        dr->tex_rt[u]=-1;
        if(mask&(1u<<u)) { port_texture_prepared_units++;resolve(dr,draw,u); }
        else { port_texture_skipped_units++;null_view(dr,draw,u); }
    }
}
static void port_null_texture_unit(D3D12DrawRecord* dr,u32 draw,int unit)
{
    u32 slot=DRAW_SRV_BASE+draw*PORT_TEXTURE_UNITS+(u32)unit;
    /* Retain the view dimension expected by the selected shader's cube key. */
    srv_write_ex(slot,NULL,DXGI_FORMAT_R8G8B8A8_UNORM,
                 D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,(dr_cube_mask(dr)>>unit)&1u);
}
static void port_resolve_texture_unit(D3D12DrawRecord* dr,u32 _d,int _u)
{
'''+body+r'''
}
'''
    fixture=(Path(__file__).resolve().parents[1]/'port/checks/texture_prepass.inc').read_text(encoding='utf-8')
    once('static void render_frame(void)',helper+'\n'+fixture+'\nstatic void render_frame(void)')
    return text
