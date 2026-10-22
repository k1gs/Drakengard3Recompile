"""Prepare gamepad support and existing keyboard bindings in a build-local copy."""
import re
from pathlib import Path
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
    # Keep the existing Windows keyboard fallback available with SDL, without
    # changing its bindings or its priority relative to physical controllers.
    begin = text.index('/* Keyboard fallback for port 0.')
    end = text.index('static void pad_poll_xinput(void)', begin)
    keyboard = text[begin:end]
    text = text[:begin] + text[end:]
    marker = '#if PAD_BACKEND_XINPUT\n\n/* Deadzone'
    if text.count(marker) != 1:
        raise ValueError('Pinned XInput backend anchor changed')
    text = text.replace(marker, keyboard + marker)
    begin = text.index('static u8 pad_sdl_axis_to_u8(int raw)')
    end = text.index('#endif /* PAD_BACKEND_SDL2 */', begin)
    backend = Path(__file__).resolve().parents[1] / 'port/runtime/gamepad_sdl.inc'
    text = text[:begin] + '#include "' + backend.as_posix() + '"\n\n' + text[end:]
    text = text.replace('#include <SDL2/SDL.h>', '#include <SDL.h>')
    # The backend owns its handles on its worker thread.
    text = text.replace('static SDL_GameController* s_sdl_controllers[PAD_MAX_HOST_PORTS];\n'
                        'static int s_sdl_inited = 0;', '')
    text = text.replace('    u8   press_r1;\n', '    u8   press_r1;\n    u16 sensor[4];\n')
    for axis, index in (('X', 0), ('Y', 1), ('Z', 2), ('G', 3)):
        pattern = rf'(data->button\[CELL_PAD_BTN_OFFSET_SENSOR_{axis}\] = )\d+;'
        text, count = re.subn(pattern, rf'\g<1>hs->sensor[{index}];', text)
        if count != 1:
            raise ValueError('Pinned sensor anchor changed: ' + axis)
    # Retain analog trigger pressure instead of promoting every partial pull
    # to 255. Digital injected/keyboard triggers already report 0 or 255.
    old = '&& data->button[pm[k].off] < 255)'
    if text.count(old) != 1:
        raise ValueError('Pinned pressure anchor changed')
    text = text.replace(old, '&& data->button[pm[k].off] == 0)')
    write_if_changed(output / 'cellPad_compat.c', resolve_local_includes(text, source))
