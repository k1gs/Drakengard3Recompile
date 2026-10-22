"""Prepare native gamepad input and keyboard bindings without editing the SDK."""
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
    # The Windows keyboard fallback must also be available with SDL input.
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
    root = Path(__file__).resolve().parents[1]
    backend = root / 'port/runtime/gamepad_sdl.inc'
    text = text[:begin] + '#include "' + backend.as_posix() + '"\n\n' + text[end:]
    changes = (
        ('#include <SDL2/SDL.h>', '#include <SDL.h>'),
        ('static SDL_GameController* s_sdl_controllers[PAD_MAX_HOST_PORTS];\nstatic int s_sdl_inited = 0;', ''),
        ('    pad_init_backend();', '''    pad_init_backend();
#if PAD_BACKEND_SDL2
    if(!port_pad_init_ok) {
        pad_shutdown_backend();s_pad_initialized=0;
        return CELL_PAD_ERROR_FATAL;
    }
#endif'''),
    )
    for old, new in changes:
        if text.count(old) != 1:
            raise ValueError('Pinned SDL input anchor changed: ' + old)
        text = text.replace(old, new)
    begin = text.index('#if PAD_BACKEND_SDL2', text.index('s32 cellPadSetActDirect('))
    end = text.index('#endif', begin) + len('#endif')
    text = text[:begin] + '''#if PAD_BACKEND_SDL2
    return port_pad_request_rumble(port_no,
        (Uint16)(param->motor[CELL_PAD_ACTUATOR_PARAM_LARGE]*257u),
        param->motor[CELL_PAD_ACTUATOR_PARAM_SMALL]?65535u:0u);
#endif''' + text[end:]
    # A partial analog trigger retains its pressure value; injected digital
    # buttons still receive full pressure when the backend supplied none.
    old = '&& data->button[pm[k].off] < 255)'
    if text.count(old) != 1:
        raise ValueError('Pinned pressure anchor changed')
    text = text.replace(old, '&& data->button[pm[k].off] == 0)')
    checks = root / 'port/checks/gamepad_checks.inc'
    text += '\n#if PAD_BACKEND_SDL2\n#include "' + checks.as_posix() + '"\n#endif\n'
    write_if_changed(output / 'cellPad_compat.c', resolve_local_includes(text, source))
