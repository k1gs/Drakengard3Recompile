"""Upload complete guest mip/face chains through the actual D3D12 texture cache."""
from pathlib import Path


def prepare(text):
    def once(old, new, count=1):
        nonlocal text
        if text.count(old) != count:
            raise ValueError('Pinned mip-upload anchor changed: ' + old[:90])
        text = text.replace(old, new)

    once('    u32 off, w, h, fmt; /* current contents',
         '    u32 mip_levels, source_pitch;\n    u8* source_bytes;\n    size_t source_size;\n    u32 off, w, h, fmt; /* current contents')
    once('        u32 sampler_address,', '        u32 source_pitch;\n        u32 sampler_address,')
    once('fmt, ctrl1, mips, sampler_address,', 'fmt, ctrl1, mips, source_pitch, sampler_address,')
    once('        COPY_TEX(sampler_address);', '        COPY_TEX(source_pitch);COPY_TEX(sampler_address);')
    once('        s_d3d.cur_texs[unit].sampler_address=tex->address;',
         '        s_d3d.cur_texs[unit].source_pitch=tex->control3&0xFFFFFu;\n'
         '        s_d3d.cur_texs[unit].sampler_address=tex->address;')
    once('_cube, dr->tex[_u].mips);',
         '_cube, dr->tex[_u].mips, dr->tex[_u].source_pitch);')
    # All previous preparation passes have finished; replace the pinned upload
    # implementation, not vendor sources or a parallel test-only uploader.
    begin=text.index('static int vp_upload_tex_slot(')
    end=text.index('\n}\n',begin)+3
    old=text[begin:end]
    for anchor in ('td.MipLevels = 1;', 'rsx_texture_decode(', 't->csum = TEX_CSUM'):
        if anchor not in old:
            raise ValueError('Pinned legacy texture uploader changed: '+anchor)
    root=Path(__file__).resolve().parents[1]
    text=text[:begin]+(root/'port/renderer_mip_uploads.inc').read_text(encoding='utf-8')+text[end:]
    begin=text.index('    /* Release retained texture resources after the shutdown fence. */')
    end=text.index('    port_release_depth_surfaces();',begin)
    if 'free(s_d3d.vp_tex)' not in text[begin:end]:
        raise ValueError('Pinned texture shutdown changed')
    text=text[:begin]+'    port_mip_release_cache();\n'+text[end:]
    # Every SRV exposes the resource's actual chain; null and render-target
    # views remain one level because their underlying resources are one level.
    begin=text.index('static void srv_write_ex(')
    end=text.index('\n}\n',begin)+3
    block=text[begin:end]
    block=block.replace('    D3D12_SHADER_RESOURCE_VIEW_DESC sv = {0};',
        '    D3D12_RESOURCE_DESC resource_desc={0};\n'
        '    if(res) res->lpVtbl->GetDesc(res,&resource_desc);\n'
        '    UINT levels=res?resource_desc.MipLevels:1;\n'
        '    D3D12_SHADER_RESOURCE_VIEW_DESC sv = {0};')
    if block.count('MipLevels = 1;')!=2:
        raise ValueError('Pinned texture SRV level count changed')
    block=block.replace('MipLevels = 1;','MipLevels = levels;')
    text=text[:begin]+block+text[end:]
    fixture=(root/'port/checks/mip_uploads_gpu.inc').read_text(encoding='utf-8')
    once('static void render_frame(void)',fixture+'\nstatic void render_frame(void)')
    return text
