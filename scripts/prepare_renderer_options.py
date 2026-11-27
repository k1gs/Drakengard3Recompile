"""Remove CRT environment lookups from the renderer's per-draw paths."""


def prepare(text):
    def replace(old, new):
        nonlocal text
        if old not in text:
            raise ValueError('Missing renderer option call site: ' + old)
        text = text.replace(old, new)

    options = 'port_renderer_options()->'
    # Presence and parsed values are distinct. In particular FP_CONSTBUF=0
    # selects inline constants and RTT_PASS=0 selects the first pass.
    replace('getenv("FP_CONSTBUF") && getenv("FP_CONSTBUF")[0] == \'0\'',
            options + 'inline_fp_constants')
    replace('static int pick = -1;\n'
            '      if (pick < 0) { const char* e = getenv("RSX_DBG_PICK"); pick = e ? atoi(e) : -2; }',
            'const int pick = ' + options + 'debug_pick;')
    replace('int _cap = atoi(getenv("RTT_DUMP")); if (_cap < 2) _cap = 14;',
            'int _cap = ' + options + 'dump_pass_limit;')
    replace('const char* rp = getenv("RTT_PASS");\n      if (rp) {\n        int keep = atoi(rp)',
            'if (' + options + 'has_selected_pass) {\n        int keep = ' + options + 'selected_pass')
    replace('getenv("RTT_VIEWRT") != NULL', options + 'view_pass_target')
    # This option takes an address. Preserve its parser and fetch the string
    # only when the diagnostic is actually in use.
    replace('const char* sv = getenv("RTT_SAVERT");',
            'const char* sv = ' + options + 'save_target ? getenv("RTT_SAVERT") : NULL;')

    for name, field in (
        ('VTX_DUMP', 'dump_vertices'),
        ('MOVIE_BIND', 'log_movie_binds'),
        ('TEXFMTDBG', 'log_texture_formats'),
        ('NO_ALPHATEST', 'disable_alpha_test'),
        ('BLENDDBG', 'log_blend'),
        ('RTT_DUMP', 'dump_pass_limit'),
        ('RTT_SAVEA', 'save_target_alpha'),
    ):
        replace(f'getenv("{name}")', options + field)

    anchor = '#include <d3dcompiler.h>'
    replace(anchor, anchor + '\n#include "renderer_options.inc"')
    return text
