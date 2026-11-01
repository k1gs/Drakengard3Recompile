"""Opt-in PIX instrumentation, loaded before any D3D12 initialization."""
from pathlib import Path


def prepare(text):
    def once(old, new):
        nonlocal text
        if text.count(old) != 1:
            raise ValueError('Pinned PIX anchor changed: ' + old[:90])
        text = text.replace(old, new)
    helper=(Path(__file__).resolve().parents[1]/'port/renderer_pix.inc').read_text(encoding='utf-8')
    anchor='static int init_d3d12(u32 width, u32 height)\n{'
    once(anchor,helper+'\n'+anchor+'\n    port_pix_initialize();')
    once('    wait_for_gpu();\n    port_feedback_begin_batch();',
         '    wait_for_gpu();\n    port_feedback_begin_batch();\n    port_pix_poll_capture();')
    return text
