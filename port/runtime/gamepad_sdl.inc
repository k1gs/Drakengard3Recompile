/* Included by the build-local cellPad.c. SDL handles, its event pump and its
 * hidden Windows device-notification window all belong to this worker. Guest
 * PPU threads only exchange snapshots/rumble requests under a mutex. */
typedef struct { Uint16 lo,hi; Uint64 sent_ms,generation; } PortPadRumble;
#define PAD_RUMBLE_HOLD_MS 500
#define PAD_RUMBLE_RENEW_MS 100
static int port_pad_init_ok;
static int port_pad_rumble_supported[PAD_MAX_HOST_PORTS];
static SDL_mutex* port_pad_lock;
static SDL_Thread* port_pad_thread;
static SDL_sem* port_pad_ready;
static int port_pad_stop;
static PadHostState port_pad_snapshot[PAD_MAX_HOST_PORTS];
static PortPadRumble port_pad_rumble[PAD_MAX_HOST_PORTS];
static Uint64 port_pad_last_poll;

static void port_pad_neutral(PadHostState* hs)
{
    memset(hs, 0, sizeof *hs);
    hs->analog_lx = hs->analog_ly = hs->analog_rx = hs->analog_ry = 128;
}

static u8 port_pad_axis(int raw)
{
    /* Rescale outside the deadzone, preserving exact centre and both ends. */
    const int deadzone = 7849;
    if (raw < -deadzone)
        return (u8)(128 - ((-raw - deadzone) * 128 / (32768 - deadzone)));
    if (raw > deadzone)
        return (u8)(128 + ((raw - deadzone) * 127 / (32767 - deadzone)));
    return 128;
}

static u8 port_pad_trigger(int raw)
{
    if (raw <= 0) return 0;
    if (raw >= 32767) return 255;
    return (u8)(raw * 255 / 32767);
}

static void port_pad_read(SDL_GameController* gc, PadHostState* hs)
{
    static const struct { SDL_GameControllerButton host; u16 guest; } map[] = {
        { SDL_CONTROLLER_BUTTON_BACK, CELL_PAD_CTRL_SELECT },
        { SDL_CONTROLLER_BUTTON_START, CELL_PAD_CTRL_START },
        { SDL_CONTROLLER_BUTTON_LEFTSTICK, CELL_PAD_CTRL_L3 },
        { SDL_CONTROLLER_BUTTON_RIGHTSTICK, CELL_PAD_CTRL_R3 },
        { SDL_CONTROLLER_BUTTON_DPAD_UP, CELL_PAD_CTRL_UP },
        { SDL_CONTROLLER_BUTTON_DPAD_DOWN, CELL_PAD_CTRL_DOWN },
        { SDL_CONTROLLER_BUTTON_DPAD_LEFT, CELL_PAD_CTRL_LEFT },
        { SDL_CONTROLLER_BUTTON_DPAD_RIGHT, CELL_PAD_CTRL_RIGHT },
        { SDL_CONTROLLER_BUTTON_LEFTSHOULDER, CELL_PAD_CTRL_L1 },
        { SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, CELL_PAD_CTRL_R1 },
        { SDL_CONTROLLER_BUTTON_A, CELL_PAD_CTRL_CROSS },
        { SDL_CONTROLLER_BUTTON_B, CELL_PAD_CTRL_CIRCLE },
        { SDL_CONTROLLER_BUTTON_X, CELL_PAD_CTRL_SQUARE },
        { SDL_CONTROLLER_BUTTON_Y, CELL_PAD_CTRL_TRIANGLE },
        { SDL_CONTROLLER_BUTTON_TOUCHPAD, CELL_PAD_CTRL_SELECT },
    };
    port_pad_neutral(hs);
    hs->connected = 1;
    for (unsigned i = 0; i < sizeof map / sizeof map[0]; ++i)
        if (SDL_GameControllerGetButton(gc, map[i].host)) hs->buttons |= map[i].guest;
    hs->analog_lx = port_pad_axis(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTX));
    hs->analog_ly = port_pad_axis(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTY));
    hs->analog_rx = port_pad_axis(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTX));
    hs->analog_ry = port_pad_axis(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTY));
    hs->trigger_l2 = port_pad_trigger(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERLEFT));
    hs->trigger_r2 = port_pad_trigger(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERRIGHT));
    if (hs->trigger_l2 > 30) hs->buttons |= CELL_PAD_CTRL_L2;
    if (hs->trigger_r2 > 30) hs->buttons |= CELL_PAD_CTRL_R2;
#define PORT_PRESS(field, button) hs->field = (hs->buttons & (button)) ? 255 : 0
    PORT_PRESS(press_up, CELL_PAD_CTRL_UP);
    PORT_PRESS(press_down, CELL_PAD_CTRL_DOWN);
    PORT_PRESS(press_left, CELL_PAD_CTRL_LEFT);
    PORT_PRESS(press_right, CELL_PAD_CTRL_RIGHT);
    PORT_PRESS(press_triangle, CELL_PAD_CTRL_TRIANGLE);
    PORT_PRESS(press_circle, CELL_PAD_CTRL_CIRCLE);
    PORT_PRESS(press_cross, CELL_PAD_CTRL_CROSS);
    PORT_PRESS(press_square, CELL_PAD_CTRL_SQUARE);
    PORT_PRESS(press_l1, CELL_PAD_CTRL_L1);
    PORT_PRESS(press_r1, CELL_PAD_CTRL_R1);
#undef PORT_PRESS

}

static int SDLCALL port_pad_worker(void* unused)
{
    (void)unused;
    const Uint32 subsystems = SDL_INIT_GAMECONTROLLER;
    SDL_GameController* controllers[PAD_MAX_HOST_PORTS] = {0};
    PortPadRumble sent[PAD_MAX_HOST_PORTS] = {0};
    /* Explicit physical layout, including Nintendo's B/A/Y/X labels. */
    SDL_SetHintWithPriority(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, "0", SDL_HINT_OVERRIDE);
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_SWITCH, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_JOY_CONS, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_COMBINE_JOY_CONS, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (SDL_InitSubSystem(subsystems) < 0) {
        fprintf(stderr, "[cellPad] SDL gamepad initialization failed: %s\n", SDL_GetError());
        SDL_SemPost(port_pad_ready);
        return 1;
    }
    /* SDL's standard SDL_GAMECONTROLLERCONFIG and
     * SDL_GAMECONTROLLERCONFIG_FILE overrides remain available. */
    int first = 1;
    for (;;) {
        SDL_LockMutex(port_pad_lock);
        int stop = port_pad_stop;
        Uint64 last_poll = port_pad_last_poll;
        PortPadRumble requested[PAD_MAX_HOST_PORTS];
        memcpy(requested, port_pad_rumble, sizeof requested);
        SDL_UnlockMutex(port_pad_lock);
        if (stop) break;
        SDL_PumpEvents();
        SDL_GameControllerUpdate();
        /* We own only joystick/controller events, never window events. */
        SDL_FlushEvents(SDL_JOYAXISMOTION, SDL_JOYBATTERYUPDATED);
        SDL_FlushEvents(SDL_CONTROLLERAXISMOTION, SDL_CONTROLLERSENSORUPDATE);
        int changed[PAD_MAX_HOST_PORTS] = {0};
        for (int p = 0; p < PAD_MAX_HOST_PORTS; ++p) {
            if (controllers[p] && !SDL_GameControllerGetAttached(controllers[p])) {
                SDL_GameControllerRumble(controllers[p], 0, 0, 0);
                SDL_GameControllerClose(controllers[p]);
                controllers[p] = NULL;
                changed[p] = 1;
            }
        }
        /* Device indices move on removal. Bind ports to instance IDs and fill
         * free ports by scanning ALL devices, including past non-gamepads. */
        for (int device = 0; device < SDL_NumJoysticks(); ++device) {
            if (!SDL_IsGameController(device)) continue;
            SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(device);
            int assigned = 0, free_port = -1;
            for (int p = 0; p < PAD_MAX_HOST_PORTS; ++p) {
                if (controllers[p]) {
                    if (SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(controllers[p])) == id)
                        assigned = 1;
                } else if (free_port < 0) free_port = p;
            }
            if (assigned || free_port < 0) continue;
            SDL_GameController* gc = SDL_GameControllerOpen(device);
            if (!gc) continue;
            controllers[free_port] = gc;
            changed[free_port] = 1;
            fprintf(stderr, "[cellPad] port %d: %s (SDL type %d)\n", free_port,
                    SDL_GameControllerName(gc), (int)SDL_GameControllerGetType(gc));
        }
        PadHostState snapshot[PAD_MAX_HOST_PORTS];
        Uint64 now = SDL_GetTicks64();
        for (int p = 0; p < PAD_MAX_HOST_PORTS; ++p) {
            port_pad_neutral(&snapshot[p]);
            if (changed[p]) {
                memset(&requested[p], 0, sizeof requested[p]);
                memset(&sent[p], 0, sizeof sent[p]);
            }
            if (!controllers[p]) continue;
            port_pad_read(controllers[p], &snapshot[p]);
            /* Don't renew indefinitely if the guest stops consuming input. */
            if (now - last_poll >= PAD_RUMBLE_HOLD_MS)
                requested[p].lo = requested[p].hi = 0;
            if (requested[p].lo != sent[p].lo || requested[p].hi != sent[p].hi ||
                ((requested[p].lo || requested[p].hi) && now - sent[p].sent_ms >= PAD_RUMBLE_RENEW_MS)) {
                SDL_GameControllerRumble(controllers[p], requested[p].lo, requested[p].hi,
                                         (requested[p].lo || requested[p].hi) ? PAD_RUMBLE_HOLD_MS : 0);
                sent[p] = requested[p];
                sent[p].sent_ms = now;
            }
        }
        SDL_LockMutex(port_pad_lock);
        memcpy(port_pad_snapshot, snapshot, sizeof snapshot);
        /* Consume expired requests, so a later input poll cannot restart them.
         * A newer guest command published during this iteration stays intact. */
        if(now>=port_pad_last_poll && now-port_pad_last_poll>=PAD_RUMBLE_HOLD_MS) {
            for(int p=0;p<PAD_MAX_HOST_PORTS;p++)
                if(port_pad_rumble[p].generation==requested[p].generation)
                    port_pad_rumble[p].lo=port_pad_rumble[p].hi=0;
        }
        for(int p=0;p<PAD_MAX_HOST_PORTS;p++)
            port_pad_rumble_supported[p]=controllers[p] && SDL_GameControllerHasRumble(controllers[p]);
        for (int p = 0; p < PAD_MAX_HOST_PORTS; ++p)
            if (changed[p]) memset(&port_pad_rumble[p], 0, sizeof port_pad_rumble[p]);
        SDL_UnlockMutex(port_pad_lock);
        if (first) { first = 0; port_pad_init_ok=1; SDL_SemPost(port_pad_ready); }
        SDL_Delay(4);
    }
    for (int p = 0; p < PAD_MAX_HOST_PORTS; ++p) {
        if (!controllers[p]) continue;
        SDL_GameControllerRumble(controllers[p], 0, 0, 0);
        SDL_GameControllerClose(controllers[p]);
    }
    SDL_QuitSubSystem(subsystems);
    return 0;
}

static void pad_poll_sdl2(void)
{
    if (!port_pad_lock) return;
    SDL_LockMutex(port_pad_lock);
    memcpy(s_host_state, port_pad_snapshot, sizeof s_host_state);
    port_pad_last_poll = SDL_GetTicks64();
    SDL_UnlockMutex(port_pad_lock);
}

static s32 port_pad_request_rumble(unsigned port,Uint16 low,Uint16 high)
{
    if(!port_pad_lock || port>=PAD_MAX_HOST_PORTS) return CELL_PAD_ERROR_NO_DEVICE;
    SDL_LockMutex(port_pad_lock);
    s32 result=CELL_OK;
    if(!port_pad_snapshot[port].connected) result=CELL_PAD_ERROR_NO_DEVICE;
    else if(!port_pad_rumble_supported[port]) result=CELL_PAD_ERROR_FATAL;
    else {
        port_pad_rumble[port].lo=low;port_pad_rumble[port].hi=high;
        port_pad_rumble[port].generation++;
    }
    SDL_UnlockMutex(port_pad_lock);
    return result;
}

static void pad_init_backend(void)
{
    /* The port has its own entry point and does not link SDL2main. */
    SDL_SetMainReady();
    for (int p = 0; p < PAD_MAX_HOST_PORTS; ++p) {
        port_pad_neutral(&port_pad_snapshot[p]);
        port_pad_neutral(&s_host_state[p]);
    }
    memset(port_pad_rumble_supported,0,sizeof port_pad_rumble_supported);
    memset(port_pad_rumble, 0, sizeof port_pad_rumble);
    port_pad_stop = 0;
    port_pad_init_ok=0;
    port_pad_last_poll = SDL_GetTicks64();
    port_pad_lock = SDL_CreateMutex();
    port_pad_ready = SDL_CreateSemaphore(0);
    if (port_pad_lock && port_pad_ready)
        port_pad_thread = SDL_CreateThread(port_pad_worker, "gamepad", NULL);
    if (port_pad_thread) SDL_SemWait(port_pad_ready);
    else {
        fprintf(stderr, "[cellPad] SDL gamepad worker failed: %s\n", SDL_GetError());
        if (port_pad_lock) SDL_DestroyMutex(port_pad_lock);
        port_pad_lock = NULL;
    }
    if (port_pad_ready) SDL_DestroySemaphore(port_pad_ready);
    port_pad_ready = NULL;
}

static void pad_shutdown_backend(void)
{
    if (port_pad_thread) {
        SDL_LockMutex(port_pad_lock);
        port_pad_stop = 1;
        SDL_UnlockMutex(port_pad_lock);
        SDL_WaitThread(port_pad_thread, NULL);
        port_pad_thread = NULL;
    }
    if (port_pad_lock) SDL_DestroyMutex(port_pad_lock);
    port_pad_lock = NULL;
    port_pad_init_ok=0;
    memset(port_pad_rumble_supported,0,sizeof port_pad_rumble_supported);
    for (int p = 0; p < PAD_MAX_HOST_PORTS; ++p) port_pad_neutral(&s_host_state[p]);
}
