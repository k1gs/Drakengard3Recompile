/* Real SDL worker and cellPad ABI contract, using SDL virtual devices only.
 * This does not validate the target controller's Bluetooth transport. */
#define PAD_CHECK(condition) do { if(!(condition)) { \
    fprintf(stderr,"[gamepad-check] line %d: %s failed\n",__LINE__,#condition); \
    exit(1); } } while(0)
static SDL_atomic_t rumble_low, rumble_high;
static int SDLCALL rumble(void* data, Uint16 low, Uint16 high)
{
    (void)data;
    SDL_AtomicSet(&rumble_low, low);
    SDL_AtomicSet(&rumble_high, high);
    return 0;
}

static SDL_Joystick* attach(const char* name, Uint16 vendor, Uint16 product, int gamepad)
{
    SDL_VirtualJoystickDesc desc;
    SDL_zero(desc);
    desc.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
    desc.type = gamepad ? SDL_JOYSTICK_TYPE_GAMECONTROLLER : SDL_JOYSTICK_TYPE_FLIGHT_STICK;
    desc.naxes = SDL_CONTROLLER_AXIS_MAX;
    desc.nbuttons = SDL_CONTROLLER_BUTTON_MAX;
    desc.vendor_id = vendor;
    desc.product_id = product;
    desc.name = name;
    desc.Rumble = rumble;
    SDL_LockJoysticks();
    int index = SDL_JoystickAttachVirtualEx(&desc);
    PAD_CHECK(index >= 0);
    SDL_Joystick* js = SDL_JoystickOpen(index);
    PAD_CHECK(js);
    if (gamepad) {
        char guid[33], mapping[1024];
        SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(js), guid, sizeof guid);
        SDL_snprintf(mapping, sizeof mapping,
            "%s,%s,a:b0,b:b1,x:b2,y:b3,back:b4,guide:b5,start:b6,leftstick:b7,rightstick:b8,"
            "leftshoulder:b9,rightshoulder:b10,dpup:b11,dpdown:b12,dpleft:b13,dpright:b14,"
            "touchpad:b20,leftx:a0,lefty:a1,rightx:a2,righty:a3,lefttrigger:a4,righttrigger:a5,",
            guid, name);
        PAD_CHECK(SDL_GameControllerAddMapping(mapping) >= 0);
    }
    /* Full signed joystick axes map to nonnegative controller triggers. */
    PAD_CHECK(SDL_JoystickSetVirtualAxis(js, 4, -32768) == 0);
    PAD_CHECK(SDL_JoystickSetVirtualAxis(js, 5, -32768) == 0);
    SDL_UnlockJoysticks();
    return js;
}

static void detach(SDL_Joystick* js)
{
    SDL_LockJoysticks();
    SDL_JoystickID id = SDL_JoystickInstanceID(js);
    int found = 0;
    for (int i = 0; i < SDL_NumJoysticks(); ++i) {
        if (SDL_JoystickGetDeviceInstanceID(i) != id) continue;
        PAD_CHECK(SDL_JoystickDetachVirtual(i) == 0);
        found = 1;
        break;
    }
    PAD_CHECK(found);
    SDL_JoystickClose(js);
    SDL_UnlockJoysticks();
}

static void wait_state(int port, int connected, u16 buttons)
{
    for (int n = 0; n < 500; ++n) {
        pad_poll_sdl2();
        if (s_host_state[port].connected == connected && s_host_state[port].buttons == buttons)
            return;
        SDL_Delay(4);
    }
    fprintf(stderr, "port %d expected connected=%d buttons=%04x, got %d/%04x\n",
            port, connected, buttons, s_host_state[port].connected, s_host_state[port].buttons);
    PAD_CHECK(0);
}

static u16 read_word(int offset)
{
    /* Force one fresh guest report; test the big-endian HLE output. */
    s_data_fresh[0] = 1;
    PAD_CHECK(cellPadGetData(0, (CellPadData*)(uintptr_t)0x100) == CELL_OK);
    return vm_read16(0x104 + 2 * offset);
}

int port_check_gamepad(void)
{
    PAD_CHECK(port_pad_axis(-32768) == 0);
    PAD_CHECK(port_pad_axis(32767) == 255);
    PAD_CHECK(port_pad_axis(0) == 128);
    PAD_CHECK(port_pad_axis(-7849) == 128 && port_pad_axis(7849) == 128);
    PAD_CHECK(port_pad_trigger(-1) == 0 && port_pad_trigger(32767) == 255);
    /* Isolate deterministic virtual input from physical controllers. */
    static const char* hints[]={SDL_HINT_JOYSTICK_HIDAPI,SDL_HINT_XINPUT_ENABLED,
        SDL_HINT_JOYSTICK_RAWINPUT,SDL_HINT_JOYSTICK_WGI,SDL_HINT_DIRECTINPUT_ENABLED};
    for(unsigned i=0;i<sizeof hints/sizeof hints[0];i++)
        SDL_SetHintWithPriority(hints[i],"0",SDL_HINT_OVERRIDE);
    PAD_CHECK(cellPadInit(4) == CELL_OK);
    PAD_CHECK(!SDL_GetHintBoolean(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS,SDL_TRUE));
    /* Keep the test-owned virtual joystick handles alive across cellPadEnd. */
    PAD_CHECK(SDL_InitSubSystem(SDL_INIT_JOYSTICK) == 0);
    wait_state(0, 0, 0);
    PAD_CHECK(read_word(CELL_PAD_BTN_OFFSET_ANALOG_LEFT_X) == 128);
    PAD_CHECK(cellPadSetPortSetting(0, CELL_PAD_SETTING_PRESS_ON | CELL_PAD_SETTING_SENSOR_ON) == CELL_OK);
    PAD_CHECK(read_word(CELL_PAD_BTN_OFFSET_SENSOR_Y) == 399);

    /* A non-gamepad ahead of the pads must not consume a player port. */
    SDL_Joystick* flight = attach("Flight stick", 0x1234, 1, 0);
    SDL_Joystick* pad = attach("Test positional controller", 0x1234, 2, 1);
    wait_state(0, 1, 0);
    static const u16 buttons[] = {
        CELL_PAD_CTRL_CROSS, CELL_PAD_CTRL_CIRCLE, CELL_PAD_CTRL_SQUARE, CELL_PAD_CTRL_TRIANGLE,
        CELL_PAD_CTRL_SELECT, 0, CELL_PAD_CTRL_START, CELL_PAD_CTRL_L3, CELL_PAD_CTRL_R3,
        CELL_PAD_CTRL_L1, CELL_PAD_CTRL_R1, CELL_PAD_CTRL_UP, CELL_PAD_CTRL_DOWN,
        CELL_PAD_CTRL_LEFT, CELL_PAD_CTRL_RIGHT
    };
    for (unsigned b = 0; b < sizeof buttons / sizeof buttons[0]; ++b) {
        if (!buttons[b]) continue;
        PAD_CHECK(SDL_JoystickSetVirtualButton(pad, b, 1) == 0);
        wait_state(0, 1, buttons[b]);
        PAD_CHECK((read_word(CELL_PAD_BTN_OFFSET_DIGITAL1) |
               (read_word(CELL_PAD_BTN_OFFSET_DIGITAL2) << 8)) == buttons[b]);
        PAD_CHECK(SDL_JoystickSetVirtualButton(pad, b, 0) == 0);
        wait_state(0, 1, 0);
    }
    SDL_JoystickSetVirtualButton(pad, 20, 1);
    wait_state(0, 1, CELL_PAD_CTRL_SELECT);
    SDL_JoystickSetVirtualButton(pad, 20, 0);
    SDL_JoystickSetVirtualAxis(pad, 0, -32768);
    SDL_JoystickSetVirtualAxis(pad, 1, 32767);
    SDL_JoystickSetVirtualAxis(pad, 2, 32767);
    SDL_JoystickSetVirtualAxis(pad, 3, -32768);
    SDL_JoystickSetVirtualAxis(pad, 4, 0); /* half pull */
    SDL_JoystickSetVirtualAxis(pad, 5, 32767);
    wait_state(0, 1, CELL_PAD_CTRL_L2 | CELL_PAD_CTRL_R2);
    PAD_CHECK(read_word(CELL_PAD_BTN_OFFSET_ANALOG_LEFT_X) == 0);
    PAD_CHECK(read_word(CELL_PAD_BTN_OFFSET_ANALOG_LEFT_Y) == 255);
    PAD_CHECK(read_word(CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_X) == 255);
    PAD_CHECK(read_word(CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_Y) == 0);
    u16 pressure = read_word(CELL_PAD_BTN_OFFSET_PRESS_L2);
    PAD_CHECK(pressure >= 126 && pressure <= 128);
    PAD_CHECK(read_word(CELL_PAD_BTN_OFFSET_PRESS_R2) == 255);

    vm_base[0x400] = 1; vm_base[0x401] = 128;
    PAD_CHECK(cellPadSetActDirect(0, (CellPadActParam*)(uintptr_t)0x400) == CELL_OK);
    for (int n = 0; n < 500 && SDL_AtomicGet(&rumble_high) != 65535; ++n) SDL_Delay(4);
    PAD_CHECK(SDL_AtomicGet(&rumble_low) == 128 * 257);
    PAD_CHECK(SDL_AtomicGet(&rumble_high) == 65535);
    /* A stalled guest must not leave the motors running indefinitely. */
    for (int n = 0; n < 500 && SDL_AtomicGet(&rumble_high); ++n) SDL_Delay(4);
    PAD_CHECK(SDL_AtomicGet(&rumble_low) == 0 && SDL_AtomicGet(&rumble_high) == 0);
    /* Resuming input must not revive a vibration command that expired. */
    pad_poll_sdl2();SDL_Delay(30);
    PAD_CHECK(SDL_AtomicGet(&rumble_low) == 0 && SDL_AtomicGet(&rumble_high) == 0);

    SDL_Joystick* nintendo = attach("Switch Pro Controller", 0x057e, 0x2009, 1);
    SDL_Joystick* xbox = attach("Xbox Series Controller", 0x045e, 0x0b13, 1);
    wait_state(1, 1, 0);
    wait_state(2, 1, 0);
    detach(pad);
    wait_state(0, 0, 0);
    PAD_CHECK(s_host_state[0].trigger_l2 == 0 && s_host_state[0].analog_lx == 128);
    /* Removing an earlier SDL device cannot move surviving controller slots. */
    SDL_JoystickSetVirtualButton(nintendo, 0, 1);
    SDL_JoystickSetVirtualButton(xbox, 1, 1);
    wait_state(1, 1, CELL_PAD_CTRL_CROSS);
    wait_state(2, 1, CELL_PAD_CTRL_CIRCLE);
    pad = attach("Test positional controller", 0x1234, 2, 1);
    wait_state(0, 1, 0);
    SDL_LockMutex(port_pad_lock);
    PAD_CHECK(port_pad_rumble[0].lo == 0 && port_pad_rumble[0].hi == 0);
    SDL_UnlockMutex(port_pad_lock);
    /* The callback for the unplugged virtual device cannot receive motor-off.
     * Observe fresh callbacks from the newly connected instance. */
    SDL_AtomicSet(&rumble_low, 0);
    SDL_AtomicSet(&rumble_high, 0);
    /* Explicit motor-off requests must take effect before the timeout. */
    PAD_CHECK(cellPadSetActDirect(0, (CellPadActParam*)(uintptr_t)0x400) == CELL_OK);
    for (int n = 0; n < 500 && SDL_AtomicGet(&rumble_high) != 65535; ++n) SDL_Delay(4);
    PAD_CHECK(SDL_AtomicGet(&rumble_high) == 65535);
    vm_base[0x400] = vm_base[0x401] = 0;
    PAD_CHECK(cellPadSetActDirect(0, (CellPadActParam*)(uintptr_t)0x400) == CELL_OK);
    for (int n = 0; n < 500 && SDL_AtomicGet(&rumble_high); ++n) SDL_Delay(4);
    PAD_CHECK(SDL_AtomicGet(&rumble_low) == 0 && SDL_AtomicGet(&rumble_high) == 0);
    /* Shutdown must stop an actively rumbling, still attached device. */
    vm_base[0x400] = 1; vm_base[0x401] = 255;
    PAD_CHECK(cellPadSetActDirect(0, (CellPadActParam*)(uintptr_t)0x400) == CELL_OK);
    for (int n = 0; n < 500 && SDL_AtomicGet(&rumble_high) != 65535; ++n) SDL_Delay(4);
    PAD_CHECK(SDL_AtomicGet(&rumble_high) == 65535);
    PAD_CHECK(cellPadEnd() == CELL_OK);
    PAD_CHECK(SDL_AtomicGet(&rumble_low) == 0 && SDL_AtomicGet(&rumble_high) == 0);
    PAD_CHECK(cellPadInit(4) == CELL_OK);
    /* A new API session assigns ports in the current enumeration order. */
    wait_state(0, 1, CELL_PAD_CTRL_CROSS);
    detach(pad); detach(nintendo); detach(xbox); detach(flight);
    wait_state(0, 0, 0);
    PAD_CHECK(cellPadEnd() == CELL_OK);
    PAD_CHECK(!port_pad_thread && !port_pad_lock);
    PAD_CHECK(cellPadInit(1) == CELL_OK);
    PAD_CHECK(read_word(CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_Y) == 128);
    PAD_CHECK(cellPadEnd() == CELL_OK);
    SDL_QuitSubSystem(SDL_INIT_JOYSTICK);
    puts("gamepad contract passed");
    return 0;
}

/* Explicit, bounded hardware probe: never runs as part of asset-free checks. */
int port_probe_gamepad(void)
{
    if(cellPadInit(4)!=CELL_OK) return 1;
    Uint64 start=SDL_GetTicks64();int found=0;
    PadHostState previous[PAD_MAX_HOST_PORTS];memset(previous,0,sizeof previous);
    while(SDL_GetTicks64()-start<30000) {
        pad_poll_sdl2();
        for(int p=0;p<PAD_MAX_HOST_PORTS;p++) {
            PadHostState* hs=&s_host_state[p];
            if(hs->connected) found=1;
            if(memcmp(hs,&previous[p],sizeof *hs)) {
                fprintf(stderr,"[pad-probe] port=%d connected=%d buttons=%04X left=%u,%u right=%u,%u triggers=%u,%u\n",
                    p,hs->connected,hs->buttons,hs->analog_lx,hs->analog_ly,
                    hs->analog_rx,hs->analog_ry,hs->trigger_l2,hs->trigger_r2);
                previous[p]=*hs;
            }
        }
        SDL_Delay(10);
    }
    cellPadEnd();
    fprintf(stderr,"[pad-probe] physical controller detected=%d\n",found);
    return found?0:2;
}
