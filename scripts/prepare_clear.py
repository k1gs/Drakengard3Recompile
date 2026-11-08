"""Replay guest clear rectangles and channel/stencil masks without draw-state leaks."""
from pathlib import Path


def prepare(text):
    def once(old, new):
        nonlocal text
        if text.count(old) != 1:
            raise ValueError('Pinned clear anchor changed: ' + old[:100])
        text = text.replace(old, new)

    root = Path(__file__).resolve().parents[1]
    once('    u8 clear_stencil;', '    u8 clear_stencil;\n    u8 clear_stencil_mask;')
    once('''    if((flags&0xF0u) && (flags&0xF0u)!=0xF0u) {
        fprintf(stderr,"[clear] unsupported partial color mask %02X\\n",flags);abort();
    }''', '''    u32 stencil_mask=s_d3d.current_rsx_state ? port_stencil.write_mask&255u : 255u;
    if(!stencil_mask) flags&=~2u;
    if(!flags) return;''')
    once('    r->clear_flags=flags;r->clear_depth=depth;r->clear_stencil=stencil;',
         '    r->clear_flags=flags;r->clear_depth=depth;r->clear_stencil=stencil;\n'
         '    r->clear_stencil_mask=(u8)stencil_mask;\n'
         '    port_snapshot_scissor(r,s_d3d.current_rsx_state);')
    # Clears do not require a guest vertex program. Keep them in the ordered
    # replay even before the first VP has compiled; only draw setup is gated.
    once('    if (s_d3d.vp_ready && s_d3d.draw_count > 0) {',
         '    if (s_d3d.draw_count > 0) {')
    once('        if (any && vpso) {', '        if (any) {')
    start = text.index('            s_d3d.cmd_list->lpVtbl->SetGraphicsRootSignature(s_d3d.cmd_list, s_d3d.vp_root_sig);')
    end = text.index('            D3D12_GPU_DESCRIPTOR_HANDLE gh_base;', start)
    text = text[:start] + '            if(s_d3d.vp_ready && vpso) {\n' + text[start:end] + '            }\n' + text[end:]
    once('                if (!dr->is_vp) continue;',
         '                if (!dr->is_vp || (!dr->is_clear && (!s_d3d.vp_ready || !vpso))) continue;')
    start = text.index('                if (dr->is_clear) {\n                    UINT clear_depth_flags=port_depth_clear_flags(dr);')
    end = text.index('                /* Per-draw pipeline:', start)
    text = text[:start] + '''                if (dr->is_clear) {
                    if(dsv_handle.ptr) port_clear_depth_view(dr,dsv_handle);
                    D3D12_CPU_DESCRIPTOR_HANDLE targets[4];UINT count=1;
                    targets[0]=cur_rt>=0 ? off_rt_rtv(cur_rt) : rtv_handle;
                    u32 width=cur_rt>=0 ? s_d3d.off_rt[cur_rt].w : s_d3d.width;
                    u32 height=cur_rt>=0 ? s_d3d.off_rt[cur_rt].h : s_d3d.height;
                    DXGI_FORMAT format=cur_rt>=0 ? (DXGI_FORMAT)s_d3d.off_rt[cur_rt].dxgi : DXGI_FORMAT_R8G8B8A8_UNORM;
                    port_clear_color_view(dr,targets[0],width,height,format);
                    for(int m=0;m<3;m++) if(cur_m[m]>=0) {
                        OffRT* rt=&s_d3d.off_rt[cur_m[m]];
                        targets[count]=off_rt_rtv(cur_m[m]);
                        port_clear_color_view(dr,targets[count++],rt->w,rt->h,(DXGI_FORMAT)rt->dxgi);
                    }
                    /* A masked clear uses its own root signature/OM bindings.
                     * Restore those even if the next draw has identical targets.
                     * The ordinary path sets every root argument per draw. */
                    if(s_d3d.vp_root_sig)
                        s_d3d.cmd_list->lpVtbl->SetGraphicsRootSignature(s_d3d.cmd_list,s_d3d.vp_root_sig);
                    s_d3d.cmd_list->lpVtbl->OMSetRenderTargets(s_d3d.cmd_list,count,targets,FALSE,dsv_handle.ptr?&dsv_handle:NULL);
                    continue;
                }
''' + text[end:]
    helper = (root / 'port/renderer_clear.inc').read_text(encoding='utf-8')
    check = (root / 'port/checks/clear_gpu.inc').read_text(encoding='utf-8')
    once('static void render_frame(void)', helper + '\nstatic void render_frame(void)')
    once('    /* Release D3D12 resources */',
         '    port_release_clear_pipelines();\n    /* Release D3D12 resources */')
    text += '\n' + check
    return text

def commands(text):
    old = '        if (s_backend && s_backend->clear) {'
    if text.count(old) != 1:
        raise ValueError('Pinned guest clear dispatch changed')
    return text.replace(old, '''        /* A clear may precede the first BEGIN_END. Publish its actual surface
         * state instead of inheriting a null/previous render-target callback. */
        if(s_backend && s_backend->set_render_target)
            s_backend->set_render_target(s_backend->userdata,state);
''' + old)
