"""Connect the last two RSX texture-coordinate outputs to the D3D12 pixel shader."""


def _replace(text, old, new):
    if text.count(old) != 1:
        raise ValueError(f'Pinned RSX varying source changed near {old[:70]!r}')
    return text.replace(old, new)


def prepare_vertex_outputs(sdk, output):
    text = (sdk / 'libs/video/rsx_vp_decompiler.c').read_text(encoding='utf-8')
    fields = r'float4 t7:TEXCOORD7;\n"'
    text = _replace(text, fields, fields + '\n' + r'        "    float4 t8:TEXCOORD8; float4 t9:TEXCOORD9;\n"')
    uniform = r'"    float4 vp_posoffset;\n"'
    text = _replace(text, uniform, uniform + '\n' + r'        "    uint vp_output_mask;\n"')
    outputs = r'Out.t7=o[14];\n"'
    # These output-mask bits precede TEX0. Result register 6 also serves point size.
    text = _replace(text, outputs, outputs + '\n' + r'''        "    Out.t8 = (vp_output_mask & (1u << 12)) ? o[15] : float4(0,0,0,1);\n"
        "    Out.t9 = (vp_output_mask & (1u << 13)) ? o[6] : float4(0,0,0,1);\n"''')
    target = output / 'rsx_vp_decompiler_compat.c'
    if not target.exists() or target.read_text(encoding='utf-8') != text:
        target.write_text(text, encoding='utf-8')


def prepare_fragment_inputs(text):
    fog = 'case 0x3: return "input.fog";'
    text = _replace(text, fog + '      /* FOGC */', fog + '''      /* FOGC */
    case 0xC: return "input.tc8";
    case 0xD: return "input.tc9";''')
    text = _replace(text, '/* TC8/TC9/FACING not plumbed */', '/* Unsupported fragment input */')
    end = r'''float4 tc7:TEXCOORD7;\n"
        "};\n");'''
    # Keep the input signature for programs that never consume these varyings.
    return _replace(text, end, r'''float4 tc7:TEXCOORD7;\n");
    if (strstr(out, "input.tc8") || strstr(out, "input.tc9"))
        out_puts(&p, "    float4 tc8:TEXCOORD8; float4 tc9:TEXCOORD9;\n");
    out_puts(&p, "};\n");''')


def prepare_draw_output_mask(text):
    viewport = 'float* vpx = (float*)(dst + RSX_MAX_VERTEX_CONSTANTS * 16);'
    # b0 contains the guest constants, two float4 viewport vectors and this mask.
    return _replace(text, viewport, viewport + '''
    _Static_assert(VP_CB_STRIDE >= (RSX_MAX_VERTEX_CONSTANTS + 2) * 16 + sizeof(u32),
                   "vertex output mask must fit in each draw constant-buffer slot");
    memcpy(vpx + 8, &st->vertex_attrib_output_mask, sizeof(st->vertex_attrib_output_mask));''')
