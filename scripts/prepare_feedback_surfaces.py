"""Native GPU read snapshots for colour attachments sampled by their own draw."""
from pathlib import Path


def prepare(text):
    def once(old, new):
        nonlocal text
        if text.count(old) != 1:
            raise ValueError('Pinned feedback anchor changed: ' + old[:90])
        text = text.replace(old, new)

    once('    ID3D12PipelineState* pso;\n} VPFPEntry;',
         '    ID3D12PipelineState* pso;\n    u32 sample_mask;\n} VPFPEntry;')
    once('static ID3D12PipelineState* port_find_fp_pso(', r'''
static u32 port_active_fp_samples;
static u32 port_fp_sample_mask(const char* hlsl)
{
    u32 mask=0;
    /* The pinned decompiler emits a literal array index for every texture
     * instruction. Declarations lack the dot and do not count as reads. */
    for(unsigned u=0;u<PORT_TEXTURE_UNITS;u++) {
        char needle[40];snprintf(needle,sizeof(needle),"rsx_tex[%u].",u);
        if(strstr(hlsl,needle)) mask|=1u<<u;
        snprintf(needle,sizeof(needle),"rsx_tex%u.",u);
        if(strstr(hlsl,needle)) mask|=1u<<u;
    }
    return mask;
}
static ID3D12PipelineState* port_find_fp_pso(''')
    once('q->cube_mask==key->cube_mask && q->depth_key==key->depth_key) return q->pso;',
         'q->cube_mask==key->cube_mask && q->depth_key==key->depth_key) {\n'
         '            port_active_fp_samples=q->sample_mask;return q->pso;\n        }')
    once('    if (nrt < 1) nrt = 1; if (nrt > 4) nrt = 4;',
         '    port_active_fp_samples=0;\n    if (nrt < 1) nrt = 1; if (nrt > 4) nrt = 4;')
    once('    s_d3d.vp_fp[s_d3d.vp_fp_n].pso     = pso;',
         '    port_active_fp_samples=port_fp_sample_mask(hlsl);\n'
         '    s_d3d.vp_fp[s_d3d.vp_fp_n].sample_mask=port_active_fp_samples;\n'
         '    s_d3d.vp_fp[s_d3d.vp_fp_n].pso     = pso;')
    root = Path(__file__).resolve().parents[1]
    helper = (root/'port/renderer_feedback.inc').read_text(encoding='utf-8')
    fixture = (root/'port/checks/feedback_surfaces_gpu.inc').read_text(encoding='utf-8')
    once('static void render_frame(void)',helper+'\n'+fixture+'\nstatic void render_frame(void)')
    once('    wait_for_gpu();\n\n    /* Compile the real vertex program',
         '    wait_for_gpu();\n    port_feedback_begin_batch();\n\n    /* Compile the real vertex program')
    once('''                D3D12_GPU_DESCRIPTOR_HANDLE gh = gh_base;
                gh.ptr += (u64)(DRAW_SRV_BASE + d * PORT_TEXTURE_UNITS) * s_d3d.srv_inc;''',
         '''                port_feedback_bind(dr,d,cur_rt,cur_m,dpso?port_active_fp_samples:1u);
                D3D12_GPU_DESCRIPTOR_HANDLE gh = gh_base;
                gh.ptr += (u64)(DRAW_SRV_BASE + d * PORT_TEXTURE_UNITS) * s_d3d.srv_inc;''')
    once('    wait_for_gpu();\n\n    port_release_vs_cache();',
         '    wait_for_gpu();\n\n    port_feedback_release();\n    port_release_vs_cache();')
    return text
