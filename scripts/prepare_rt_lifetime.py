"""Keep replaced RT resources alive through recording, submission and completion."""
from pathlib import Path


def prepare(text):
    def once(old, new):
        nonlocal text
        if text.count(old) != 1:
            raise ValueError('Pinned RT lifetime anchor changed: ' + old[:100])
        text = text.replace(old, new)

    root = Path(__file__).resolve().parents[1]
    helper = (root / 'port/renderer_rt_lifetime.inc').read_text(encoding='utf-8')
    once('static int off_rt_get(u32 off, u32 w, u32 h, u32 rsx_fmt)',
         helper + '\nstatic int off_rt_get(u32 off, u32 w, u32 h, u32 rsx_fmt)')
    once('    wait_for_gpu();\n    port_samplers_begin_batch();',
         '    wait_for_gpu();\n    port_release_retired_rts();\n    port_samplers_begin_batch();')
    once('    /* Release D3D12 resources */',
         '    port_destroy_retired_rts();\n    /* Release D3D12 resources */')
    check = (root / 'port/checks/rt_lifetime_gpu.inc').read_text(encoding='utf-8')
    once('static void render_frame(void)', check + '\nstatic void render_frame(void)')
    return text
