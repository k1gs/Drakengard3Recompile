"""Checked NV0039 memory-transfer preparation; report data stays unsupported."""
from pathlib import Path


def prepare(text):
    root=Path(__file__).resolve().parents[1]
    def once(old,new,count=1):
        nonlocal text
        if text.count(old)!=count:raise ValueError('Pinned NV0039 anchor changed: '+old[:90])
        text=text.replace(old,new)
    once('static void gcm_2d_method(u32 subch, u32 method, u32 data)',
         (root/'port/nv0039.inc').read_text(encoding='utf-8')+'\nstatic void gcm_2d_method(u32 subch, u32 method, u32 data)')
    anchor='    if (subch == 2 || subch == 3) {'
    once(anchor,'    if(subch==1 && port_nv0039_method(method,data))return;\n'+anchor)
    once('    memset(s_report_data, 0, sizeof(s_report_data));',
         '    port_nv0039_reset();\n    memset(s_report_data, 0, sizeof(s_report_data));',2)
    text='static void port_nv0039_reset(void);\n'+text
    return text+'\n'+(root/'port/checks/nv0039.inc').read_text(encoding='utf-8')
