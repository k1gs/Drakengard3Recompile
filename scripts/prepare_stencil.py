"""Preserve RSX stencil registers, per-draw snapshots and PSO identity."""
from pathlib import Path


def prepare(text):
    def once(old, new, count=1):
        nonlocal text
        if text.count(old) != count:
            raise ValueError('Pinned stencil anchor changed: '+old[:90])
        text=text.replace(old,new)
    pos=text.index('typedef struct {')
    text=text[:pos]+'''typedef struct {
    u32 enabled, masks, front, back;
} PortStencilKey;

'''+text[pos:]
    once('    u32 depth_key;', '    u32 depth_key;\n    PortStencilKey stencil_key;',2)
    once('    u32 sc_x, sc_y, sc_w, sc_h;', '    u32 sc_x, sc_y, sc_w, sc_h;\n    u32 stencil_ref;')
    root=Path(__file__).resolve().parents[1]
    helper=(root/'port/renderer_stencil.inc').read_text(encoding='utf-8')
    once('static void port_snapshot_textures(',helper+'\nstatic void port_snapshot_textures(')
    once('    port_snapshot_depth(dr,s_d3d.current_rsx_state);',
         '    port_snapshot_depth(dr,s_d3d.current_rsx_state);\n    port_snapshot_stencil(dr,s_d3d.current_rsx_state);')
    once('u32 cube_mask, u32 depth_key)\n{',
         'u32 cube_mask, u32 depth_key, const PortStencilKey* stencil)\n{')
    once('dr_cube_mask(dr), dr->depth_key)', 'dr_cube_mask(dr), dr->depth_key, &dr->stencil_key)',2)
    once('q->cube_mask==key->cube_mask && q->depth_key==key->depth_key)',
         'q->cube_mask==key->cube_mask && q->depth_key==key->depth_key &&\n'
         '           port_stencil_equal(&q->stencil_key,&key->stencil_key))')
    once('    key.cube_mask=cube_mask;key.depth_key=depth_key;',
         '    key.cube_mask=cube_mask;key.depth_key=depth_key;key.stencil_key=*stencil;')
    once('    s_d3d.vp_fp[s_d3d.vp_fp_n].depth_key = depth_key;',
         '    s_d3d.vp_fp[s_d3d.vp_fp_n].depth_key = depth_key;\n'
         '    s_d3d.vp_fp[s_d3d.vp_fp_n].stencil_key = *stencil;')
    once('    guest_depth_state(&pd.DepthStencilState, depth_key);\n    pd.DepthStencilState.StencilEnable = FALSE;',
         '    guest_depth_state(&pd.DepthStencilState, depth_key);\n    port_apply_stencil(&pd.DepthStencilState,stencil);')
    once('                /* Per-draw viewport: the guest rect when sane, else the',
         '                if(dr->stencil_key.enabled && !dpso) {\n'
         '                    fprintf(stderr,"[stencil-blocker] no guest FP pipeline for enabled stencil\\n");abort();\n'
         '                }\n'
         '                s_d3d.cmd_list->lpVtbl->OMSetStencilRef(s_d3d.cmd_list,dr->stencil_ref);\n'
         '                /* Per-draw viewport: the guest rect when sane, else the')
    once('    CHECK_KEY_FIELD(cull);CHECK_KEY_FIELD(cube_mask);CHECK_KEY_FIELD(depth_key);',
         '    CHECK_KEY_FIELD(cull);CHECK_KEY_FIELD(cube_mask);CHECK_KEY_FIELD(depth_key);\n'
         '    key=s_d3d.vp_fp[0];key.stencil_key.enabled=1;if(!port_find_fp_pso(&key))misses++;else ok=0;\n'
         '    key=s_d3d.vp_fp[0];key.stencil_key.masks=1;if(!port_find_fp_pso(&key))misses++;else ok=0;\n'
         '    key=s_d3d.vp_fp[0];key.stencil_key.front=1;if(!port_find_fp_pso(&key))misses++;else ok=0;\n'
         '    key=s_d3d.vp_fp[0];key.stencil_key.back=1;if(!port_find_fp_pso(&key))misses++;else ok=0;')
    fixture=(root/'port/checks/stencil_gpu.inc').read_text(encoding='utf-8')
    once('static void render_frame(void)',fixture+'\nstatic void render_frame(void)')
    return text


def commands(text):
    entry='int rsx_process_method(rsx_state* state, u32 method, u32 data)\n{'
    init='void rsx_state_init(rsx_state* state)\n{'
    if text.count(entry)!=1 or text.count(init)!=1:
        raise ValueError('Pinned stencil command entry changed')
    text=text.replace(entry,entry+'\n    if(port_renderer_stencil_method(method,data)) { state->stencil_dirty=1;return 0; }')
    text=text.replace(init,init+'\n    port_renderer_stencil_reset();')
    return 'void port_renderer_stencil_reset(void);\nint port_renderer_stencil_method(unsigned int,unsigned int);\n'+text
