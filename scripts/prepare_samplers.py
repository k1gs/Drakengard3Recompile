"""Carry fragment sampler registers into fence-owned dynamic D3D12 tables."""
from pathlib import Path


def prepare(text):
    def once(old, new, count=1):
        nonlocal text
        if text.count(old) != count:
            raise ValueError('Pinned sampler anchor changed: ' + old[:90])
        text = text.replace(old, new)

    once('        u32 ctrl1;', '        u32 sampler_address, sampler_filter, sampler_control, sampler_border;\n        u32 ctrl1;')
    once('fmt, ctrl1, mips; int cube;',
         'fmt, ctrl1, mips, sampler_address, sampler_filter, sampler_control, sampler_border; int cube;')
    once('        COPY_TEX(ctrl1);',
         '        COPY_TEX(sampler_address);COPY_TEX(sampler_filter);COPY_TEX(sampler_control);COPY_TEX(sampler_border);\n        COPY_TEX(ctrl1);')
    once('        s_d3d.cur_texs[unit].ctrl1 = tex->control1;',
         '        s_d3d.cur_texs[unit].sampler_address=tex->address;\n'
         '        s_d3d.cur_texs[unit].sampler_filter=tex->filter;\n'
         '        s_d3d.cur_texs[unit].sampler_control=tex->control0;\n'
         '        s_d3d.cur_texs[unit].sampler_border=tex->border_color;\n'
         '        s_d3d.cur_texs[unit].ctrl1 = tex->control1;')
    once('        tex.control1=0xAAE4;',
         '        tex.control1=0xAAE4;tex.address=0x00030301;\n'
         '        tex.filter=0x02060000|u;tex.control0=(12u<<7);tex.border_color=0xAABBCC00|u;')
    once('           dr.tex[u].ctrl1!=0xAAE4 || dr.tex_rt[u]!=-1)',
         '           dr.tex[u].ctrl1!=0xAAE4 || dr.tex_rt[u]!=-1 ||\n'
         '           dr.tex[u].sampler_address!=0x00030301 || dr.tex[u].sampler_filter!=(0x02060000|u) ||\n'
         '           dr.tex[u].sampler_control!=(12u<<7) || dr.tex[u].sampler_border!=(0xAABBCC00|u))')
    begin = text.index('        D3D12_STATIC_SAMPLER_DESC samp[PORT_TEXTURE_UNITS]')
    end = text.index('        D3D12_ROOT_SIGNATURE_DESC rd = {0};', begin)
    text = text[:begin] + '''        D3D12_DESCRIPTOR_RANGE sampler_range={0};
        sampler_range.RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
        sampler_range.NumDescriptors=PORT_TEXTURE_UNITS;
        rp[3].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rp[3].DescriptorTable.NumDescriptorRanges=1;
        rp[3].DescriptorTable.pDescriptorRanges=&sampler_range;
        rp[3].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;

''' + text[end:]
    once('        D3D12_ROOT_PARAMETER rp[3] = {0};', '        D3D12_ROOT_PARAMETER rp[4] = {0};')
    once('        rd.NumParameters = 3; rd.pParameters = rp;',
         '        rd.NumParameters = 4; rd.pParameters = rp;')
    once('        rd.NumStaticSamplers = PORT_TEXTURE_UNITS; rd.pStaticSamplers = samp;',
         '        rd.NumStaticSamplers = 0; rd.pStaticSamplers = NULL;')
    root = Path(__file__).resolve().parents[1]
    helper = (root/'port/renderer_samplers.inc').read_text(encoding='utf-8')
    once('static void port_snapshot_textures(', helper+'\nstatic void port_snapshot_textures(')
    anchor='                D3D12_GPU_DESCRIPTOR_HANDLE gh = gh_base;'
    once(anchor, '                port_bind_samplers(dr,dpso?port_active_fp_samples:1u);\n'+anchor)
    once('    wait_for_gpu();\n    port_feedback_begin_batch();',
         '    wait_for_gpu();\n    port_samplers_begin_batch();\n    port_feedback_begin_batch();')
    once('    wait_for_gpu();\n\n    port_feedback_release();',
         '    wait_for_gpu();\n\n    port_samplers_release();\n    port_feedback_release();')
    # The fixture is compiled against these exact generated production helpers.
    fixture=(root/'port/checks/samplers_gpu.inc').read_text(encoding='utf-8')
    once('static void render_frame(void)', fixture+'\nstatic void render_frame(void)')
    return text
