/* Included by the build-local cellPad.c. SDL handles, its event pump and its
 * hidden Windows device-notification window all belong to this worker. Guest
 * PPU threads only exchange snapshots/rumble requests under a mutex. */
static SDL_mutex* port_pad_lock;
static SDL_Thread* port_pad_thread;
static SDL_sem* port_pad_ready;
static int port_pad_stop;
static PadHostState port_pad_snapshot[PAD_MAX_HOST_PORTS];
static PadRumble port_pad_rumble[PAD_MAX_HOST_PORTS];
static Uint64 port_pad_last_poll;

static void port_pad_neutral(PadHostState* hs)
{
    memset(hs, 0, sizeof *hs);
    hs->analog_lx = hs->analog_ly = hs->analog_rx = hs->analog_ry = 128;
    hs->sensor[0] = hs->sensor[2] = hs->sensor[3] = 512;
    hs->sensor[1] = 399;
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

static u16 port_pad_sensor(float value)
{
    if (!isfinite(value)) return 512;
    if (value < 0) return 0;
    if (value > 1023) return 1023;
    return (u16)value;
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
    /* SDL SI units -> DS3's 10-bit accelerometer / yaw gyro. Pads without
     * motion sensors keep the same resting values as the old backend. */
    float v[3];
    if (SDL_GameControllerGetSensorData(gc, SDL_SENSOR_ACCEL, v, 3) == 0) {
        hs->sensor[0] = port_pad_sensor(512 - v[0] * (113.0f / SDL_STANDARD_GRAVITY));
        hs->sensor[1] = port_pad_sensor(512 - v[1] * (113.0f / SDL_STANDARD_GRAVITY));
        hs->sensor[2] = port_pad_sensor(512 - v[2] * (113.0f / SDL_STANDARD_GRAVITY));
    }
    if (SDL_GameControllerGetSensorData(gc, SDL_SENSOR_GYRO, v, 3) == 0)
        hs->sensor[3] = port_pad_sensor(512 + v[1] * (123.0f / 1.57079632679f));
}

static int SDLCALL port_pad_worker(void* unused)
{
    (void)unused;
    const Uint32 subsystems = SDL_INIT_GAMECONTROLLER | SDL_INIT_SENSOR;
    SDL_GameController* controllers[PAD_MAX_HOST_PORTS] = {0};
    PadRumble sent[PAD_MAX_HOST_PORTS] = {0};
    /* Explicit physical layout, including Nintendo's B/A/Y/X labels. */
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5_RUMBLE, "1");
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
        PadRumble requested[PAD_MAX_HOST_PORTS];
        memcpy(requested, port_pad_rumble, sizeof requested);
        SDL_UnlockMutex(port_pad_lock);
        if (stop) break;
        SDL_PumpEvents();
        SDL_GameControllerUpdate();
        /* We own only joystick/controller/sensor events, never window events. */
        SDL_FlushEvents(SDL_JOYAXISMOTION, SDL_JOYBATTERYUPDATED);
        SDL_FlushEvents(SDL_CONTROLLERAXISMOTION, SDL_CONTROLLERSENSORUPDATE);
        SDL_FlushEvent(SDL_SENSORUPDATE);
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
            if (SDL_GameControllerHasSensor(gc, SDL_SENSOR_ACCEL))
                SDL_GameControllerSetSensorEnabled(gc, SDL_SENSOR_ACCEL, SDL_TRUE);
            if (SDL_GameControllerHasSensor(gc, SDL_SENSOR_GYRO))
                SDL_GameControllerSetSensorEnabled(gc, SDL_SENSOR_GYRO, SDL_TRUE);
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
        for (int p = 0; p < PAD_MAX_HOST_PORTS; ++p)
            if (changed[p]) memset(&port_pad_rumble[p], 0, sizeof port_pad_rumble[p]);
        SDL_UnlockMutex(port_pad_lock);
        if (first) { first = 0; SDL_SemPost(port_pad_ready); }
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

static void pad_sdl_rumble_send(int port)
{
    if (!port_pad_lock) return;
    SDL_LockMutex(port_pad_lock);
    port_pad_rumble[port] = s_rumble[port];
    SDL_UnlockMutex(port_pad_lock);
}

static void pad_init_backend(void)
{
    /* The port has its own entry point and does not link SDL2main. */
    SDL_SetMainReady();
    for (int p = 0; p < PAD_MAX_HOST_PORTS; ++p) {
        port_pad_neutral(&port_pad_snapshot[p]);
        port_pad_neutral(&s_host_state[p]);
    }
    memset(s_rumble, 0, sizeof s_rumble);
    memset(port_pad_rumble, 0, sizeof port_pad_rumble);
    port_pad_stop = 0;
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
    memset(s_rumble, 0, sizeof s_rumble);
    for (int p = 0; p < PAD_MAX_HOST_PORTS; ++p) port_pad_neutral(&s_host_state[p]);
}
