"""Batch straight-line NV4097 constant uploads inside the ordinary FIFO walker."""


def prepare(text):
    def replace(old, new):
        nonlocal text
        if text.count(old) != 1:
            raise ValueError("FIFO constant packet anchor changed: " + old[:80])
        text = text.replace(old, new)

    walker = "static void gcm_rsx_process_fifo_unlocked(void)\n{"
    replace(walker, '#include "gcm_constant_packet.inc"\n'
            '#include "checks/gcm_constant_packet.inc"\n\n' + walker)
    packet = "            int port_flip_boundary = 0;"
    replace(packet, """            if (!s_sema_resume_arg &&
                port_consume_constant_packet(&s_state, s_fifo_getoff, ea, put, w)) {
                s_fifo_getoff += 4 + count * 4;
                continue;
            }
""" + packet)
    # The register dispatcher and the report unit consume the same fetched
    # word. An installed shader tracer may already name it.
    scalar = "rsx_process_method(&s_state, m, vm_read32(dea));"
    if scalar in text:
        replace(scalar, "const u32 method_value = vm_read32(dea);\n"
                        "                    rsx_process_method(&s_state, m, method_value);")
    if text.count("rsx_process_method(&s_state, m, method_value);") != 1:
        raise ValueError("Missing authoritative FIFO method value")
    replace("port_gcm_report_method(m, vm_read32(dea));", "port_gcm_report_method(m, method_value);")
    return text
