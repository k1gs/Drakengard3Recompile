"""Prepare Windows test keyboard bindings without modifying the SDK."""
from compat_source import resolve_local_includes, write_if_changed


def prepare(sdk, output):
    source = sdk / 'libs/input/cellPad.c'
    text = source.read_text(encoding='utf-8')
    changes = (
        ('    /* Digital buttons. hs->buttons packs',
         '    extern unsigned port_movie_skip_button(void);\n'
         '    const unsigned movie_start = port_movie_skip_button();\n'
         '    /* Digital buttons. hs->buttons packs'),
        ('(u16)(hs->buttons & 0xFF);', '(u16)((hs->buttons | movie_start) & 0xFF);'),
        ('{ VK_RETURN, CELL_PAD_CTRL_START },',
         '{ VK_ESCAPE, CELL_PAD_CTRL_START }, { VK_SPACE, CELL_PAD_CTRL_CROSS },\n'
         '        { VK_BACK, CELL_PAD_CTRL_CIRCLE },\n'
         '        { VK_RETURN, CELL_PAD_CTRL_START },'),
        ('if (!s_host_state[0].connected) pad_poll_keyboard();',
         'if (!s_host_state[0].connected || getenv("PORT_KEYBOARD_ONLY")) pad_poll_keyboard();'),
    )
    for old, new in changes:
        if text.count(old) != 1:
            raise ValueError('Pinned input anchor changed: ' + old)
        text = text.replace(old, new)
    write_if_changed(output / 'cellPad_compat.c', resolve_local_includes(text, source))
