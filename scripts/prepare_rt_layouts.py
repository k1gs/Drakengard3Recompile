"""Replay per-command RT layouts through GPU guest-byte backing storage."""
from pathlib import Path


def prepare(text):
    def once(old, new, count=1):
        nonlocal text
        if text.count(old) != count:
            raise ValueError('Pinned RT layout anchor changed: ' + old[:110])
        text = text.replace(old, new)

    root=Path(__file__).resolve().parents[1]
    once('    OffRT                 off_rt[MAX_OFF_RTS];',
         '    OffRT*                off_rt;\n    unsigned              off_rt_count,off_rt_capacity;')
    once('    u32                   address, source_pitch;', '''    u32                   address, source_pitch, layout_format, mips;
    ID3D12Resource*        backing;
    ID3D12Resource*        transfer;
    u64                   backing_size, transfer_size;
    D3D12_RESOURCE_STATES backing_state, transfer_state;
    int                   dirty;''')
    once('    int tex_rt[PORT_TEXTURE_UNITS];      /* pre-pass: OffRT index sampled by unit, -1 = none */',
         '    int tex_rt[PORT_TEXTURE_UNITS];\n    ID3D12Resource* tex_resource[PORT_TEXTURE_UNITS];\n    u32 texture_samples;')
    # Pool bounds are metadata counts; the old 16 is only an RTV page size.
    for old,new in [
        ('i < MAX_OFF_RTS','i < s_d3d.off_rt_count'),
        ('i<MAX_OFF_RTS','i<s_d3d.off_rt_count'),
        ('_i < MAX_OFF_RTS','_i < s_d3d.off_rt_count'),
        ('source>=MAX_OFF_RTS','(unsigned)source>=s_d3d.off_rt_count')]:
        text=text.replace(old,new)
    text=text.replace('MAX_OFF_RTS','PORT_RT_PAGE_SIZE')
    once('#define PORT_RT_PAGE_SIZE  16  /* demosaic double-buffers its 6-surface pass chain */',
         '#define PORT_RT_PAGE_SIZE 16')
    once('#define RT_SRV_BASE  5   /* SRV heap slots 5..20 hold offscreen-RT SRVs */\n','')
    start=text.index('/* Ensure an RT resource exists for this surface; (re)creates the RTV at')
    end=text.index('/* An open command list owns neither',start)
    text=text[:start]+text[end:]
    start=text.index('static int off_rt_get(u32 off, u32 address, u32 source_pitch, u32 w, u32 h, u32 rsx_fmt)')
    end=text.index('\n}\n',start)+3
    helper=(root/'port/renderer_rt_layouts.inc').read_text(encoding='utf-8')
    text=text[:start]+helper+text[end:]
    start=text.index('static D3D12_CPU_DESCRIPTOR_HANDLE off_rt_rtv(int slot)')
    end=text.index('\n}\n',start)+3
    text=text[:start]+'''static D3D12_CPU_DESCRIPTOR_HANDLE off_rt_rtv(int slot)
{
    return port_rt_rtv((unsigned)slot);
}
'''+text[end:]
    start=text.index('static void off_rt_transition(')
    end=text.index('\n}\n',start)+3
    text=text[:start]+'''static void off_rt_transition(int slot,D3D12_RESOURCE_STATES to)
{
    port_rt_native_state(&s_d3d.off_rt[slot],to);
}
'''+text[end:]
    start=text.index('    /* Render-to-texture pre-pass: make sure an offscreen RT resource exists')
    end=text.index('    /* Per-frame VP textures + guest-FP pipelines:',start)
    text=text[:start]+'''    /* Attachment layouts and resource views are resolved in GPU command
     * order below, never by the last state found in a whole-batch prepass. */
    for(unsigned i=0;i<s_d3d.off_rt_count;i++)s_d3d.off_rt[i].used=0;

'''+text[end:]
    once('''        u32 texture_mask=port_texture_prepass_mask(texture_pso);
        double _sv0 = perf_on() ? perf_now() : 0.0;
        port_prepare_texture_slots(dr,_d,texture_mask,port_resolve_texture_unit,port_null_texture_unit);
        if (perf_on()) s_perf_srv += perf_now() - _sv0;''',
         '        dr->texture_samples=port_texture_prepass_mask(texture_pso);')
    once('                const D3D12DrawRecord* dr = &s_d3d.draws[d];\n                if (!dr->is_vp || (!dr->is_clear && (!s_d3d.vp_ready || !vpso))) continue;',
         '''                D3D12DrawRecord* dr = &s_d3d.draws[d];
                if (!dr->is_vp || (!dr->is_clear && (!s_d3d.vp_ready || !vpso))) continue;
                if(!dr->is_clear) {
                    double srv_started=perf_on()?perf_now():0;
                    port_prepare_texture_slots(dr,d,dr->texture_samples,port_resolve_texture_unit,port_null_texture_unit);
                    if(perf_on())s_perf_srv+=perf_now()-srv_started;
                }''')
    once('            int cur_m[3] = {-1, -1, -1};',
         '            ID3D12Resource* cur_resources[4]={NULL,NULL,NULL,NULL};\n            int cur_m[3] = {-1, -1, -1};')
    once('                int want  = dr->rt_off  ? off_rt_find(dr->color[0].address)  : -1;',
         '''                int binding[4];port_rt_targets(dr,binding);
                int want=binding[0];''')
    start=text.index('                /* Remember where geometry actually went: off_rt_find() matches')
    end=text.index('                if (want >= 0)',start)
    text=text[:start]+text[end:]
    once('                    wantm[_m] = dr->rt_mrt[_m] ? off_rt_find(dr->color[_m+1].address) : -1;',
         '                    wantm[_m]=binding[_m+1];')
    once('                        off_rt_transition(want, D3D12_RESOURCE_STATE_RENDER_TARGET);\n','')
    once('                        off_rt_transition(wantm[_m], D3D12_RESOURCE_STATE_RENDER_TARGET);\n','')
    once('''                if (want != cur_rt || wantm[0] != cur_m[0] ||
                    wantm[1] != cur_m[1] || wantm[2] != cur_m[2] || depth_changed) {''',
         '''                int resource_changed=(want>=0?s_d3d.off_rt[want].res:NULL)!=cur_resources[0];
                for(unsigned m=0;m<3;m++)if((wantm[m]>=0?s_d3d.off_rt[wantm[m]].res:NULL)!=cur_resources[m+1])resource_changed=1;
                if (want != cur_rt || wantm[0] != cur_m[0] ||
                    wantm[1] != cur_m[1] || wantm[2] != cur_m[2] || depth_changed || resource_changed) {''')
    once('                    cur_rt = want;',
         '                    cur_resources[0]=want>=0?s_d3d.off_rt[want].res:NULL;\n'
         '                    for(unsigned m=0;m<3;m++)cur_resources[m+1]=wantm[m]>=0?s_d3d.off_rt[wantm[m]].res:NULL;\n'
         '                    cur_rt = want;')
    once('                    if(dsv_handle.ptr) port_clear_depth_view(dr,dsv_handle);',
         '                    if(dr->clear_flags&0xF0u){port_rt_mark_dirty(cur_rt);for(unsigned m=0;m<3;m++)port_rt_mark_dirty(cur_m[m]);}\n'
         '                    if(dsv_handle.ptr) port_clear_depth_view(dr,dsv_handle);')
    once('''                s_d3d.cmd_list->lpVtbl->DrawInstanced(s_d3d.cmd_list,
                    dr->vertex_count, 1, dr->vb_byte_offset / 256, 0);''',
         '''                s_d3d.cmd_list->lpVtbl->DrawInstanced(s_d3d.cmd_list,
                    dr->vertex_count, 1, dr->vb_byte_offset / 256, 0);
                if(dr->cmask){port_rt_mark_dirty(cur_rt);for(unsigned m=0;m<3;m++)port_rt_mark_dirty(cur_m[m]);}''')
    once('''                int rt = off_rt_find(dr->tex[_u].off);
                if (rt >= 0) {
                    dr->tex_rt[_u] = rt;
                    srv_write(wslot, s_d3d.off_rt[rt].res, (DXGI_FORMAT)s_d3d.off_rt[rt].dxgi,''',
         '''                int rt = port_rt_texture_view(dr,(unsigned)_u);
                if (rt >= 0) {
                    off_rt_transition(rt,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                    dr->tex_rt[_u] = rt;dr->tex_resource[_u]=s_d3d.off_rt[rt].res;
                    srv_write(wslot, s_d3d.off_rt[rt].res, (DXGI_FORMAT)s_d3d.off_rt[rt].dxgi,''')
    once('            dr->tex_rt[_u] = -1;',
         '            dr->tex_rt[_u] = -1;dr->tex_resource[_u]=NULL;')
    # Layout conversion produces native RGBA lanes. The guest crossbar is a
    # per-sampler view, including when the resource came from a GPU producer.
    once('''                    srv_write(wslot, s_d3d.off_rt[rt].res, (DXGI_FORMAT)s_d3d.off_rt[rt].dxgi,
                              D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING);''',
         '''                    srv_write(wslot, s_d3d.off_rt[rt].res, (DXGI_FORMAT)s_d3d.off_rt[rt].dxgi,
                              rsx_remap_to_d3d(dr->tex[_u].ctrl1,dr->tex[_u].fmt&0x9Fu));''')
    once('        if(!(used&(1u<<u)) || !port_feedback_alias(source,color,mrt)) continue;',
         '        if(!(used&(1u<<u)) || !port_feedback_alias(source,color,mrt) ||\n'
         '           dr->tex_resource[u]!=s_d3d.off_rt[source].res) continue;')
    once('''                    if (rt >= 0 && rt != cur_rt &&
                        rt != cur_m[0] && rt != cur_m[1] && rt != cur_m[2])''',
         '''                    if (rt >= 0 && dr->tex_resource[_u]==s_d3d.off_rt[rt].res && rt != cur_rt &&
                        rt != cur_m[0] && rt != cur_m[1] && rt != cur_m[2])''')
    once('    port_release_retired_rts();\n    port_samplers_begin_batch();',
         '    port_release_retired_rts();\n    port_rt_buffers_decay();\n    port_samplers_begin_batch();')
    once('    /* Release D3D12 resources */',
         '    port_rt_storage_release();\n    /* Release D3D12 resources */')
    text+='\n'+(root/'port/checks/rt_layouts_gpu.inc').read_text(encoding='utf-8')
    return text
