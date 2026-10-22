/* Exercises the actual patched cellPad HLE and SDL worker, without game files.
 * Virtual joysticks cover input/rumble/hotplug; physical HID transports still
 * require testing on Windows with the target controllers. */
#include <assert.h>
#include PORT_CELL_PAD_SOURCE

static u8 guest[8192];
u8* vm_base = guest;
/* No guest CPU or SPU reservations exist in this standalone HLE test. */
u32 g_ww_lo, g_ww_hi;
int g_resv_store_active;
int spu_coh_is_reserved(u32 addr) { (void)addr; return 0; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}
void spu_coh_notify_write(u32 addr) { (void)addr; }
void ppu_resv_break_store(u64 addr) { (void)addr; }
void ps3_ww_report_inline(u32 addr, u64 val, int width)
{ (void)addr; (void)val; (void)width; }
u32 cellGcm_flip_request_count(void) { return 0; }
unsigned port_movie_skip_button(void) { return 0; }
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
    assert(index >= 0);
    SDL_Joystick* js = SDL_JoystickOpen(index);
    assert(js);
    if (gamepad) {
        char guid[33], mapping[1024];
        SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(js), guid, sizeof guid);
        SDL_snprintf(mapping, sizeof mapping,
            "%s,%s,a:b0,b:b1,x:b2,y:b3,back:b4,guide:b5,start:b6,leftstick:b7,rightstick:b8,"
            "leftshoulder:b9,rightshoulder:b10,dpup:b11,dpdown:b12,dpleft:b13,dpright:b14,"
            "touchpad:b20,leftx:a0,lefty:a1,rightx:a2,righty:a3,lefttrigger:a4,righttrigger:a5,",
            guid, name);
        assert(SDL_GameControllerAddMapping(mapping) >= 0);
    }
    /* Full signed joystick axes map to nonnegative controller triggers. */
    assert(SDL_JoystickSetVirtualAxis(js, 4, -32768) == 0);
    assert(SDL_JoystickSetVirtualAxis(js, 5, -32768) == 0);
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
        assert(SDL_JoystickDetachVirtual(i) == 0);
        found = 1;
        break;
    }
    assert(found);
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
    assert(0);
}

static u16 read_word(int offset)
{
    /* Force one fresh guest report; test the big-endian HLE output. */
    s_data_fresh[0] = 1;
    assert(cellPadGetData(0, (CellPadData*)(uintptr_t)0x100) == CELL_OK);
    return vm_read16(0x104 + 2 * offset);
}

int main(void)
{
    assert(port_pad_axis(-32768) == 0);
    assert(port_pad_axis(32767) == 255);
    assert(port_pad_axis(0) == 128);
    assert(port_pad_axis(-7849) == 128 && port_pad_axis(7849) == 128);
    assert(port_pad_trigger(-1) == 0 && port_pad_trigger(32767) == 255);
    assert(port_pad_sensor(-10) == 0 && port_pad_sensor(2000) == 1023);
    assert(port_pad_sensor(NAN) == 512);
    assert(cellPadInit(4) == CELL_OK);
    /* Keep the test-owned virtual joystick handles alive across cellPadEnd. */
    assert(SDL_InitSubSystem(SDL_INIT_JOYSTICK) == 0);
    wait_state(0, 0, 0);
    assert(read_word(CELL_PAD_BTN_OFFSET_ANALOG_LEFT_X) == 128);
    assert(cellPadSetPortSetting(0, CELL_PAD_SETTING_PRESS_ON | CELL_PAD_SETTING_SENSOR_ON) == CELL_OK);
    assert(read_word(CELL_PAD_BTN_OFFSET_SENSOR_Y) == 399);

    /* A non-gamepad ahead of the pads must not consume a player port. */
    SDL_Joystick* flight = attach("Flight stick", 0x1234, 1, 0);
    SDL_Joystick* ps5 = attach("DualSense", 0x054c, 0x0ce6, 1);
    wait_state(0, 1, 0);
    static const u16 buttons[] = {
        CELL_PAD_CTRL_CROSS, CELL_PAD_CTRL_CIRCLE, CELL_PAD_CTRL_SQUARE, CELL_PAD_CTRL_TRIANGLE,
        CELL_PAD_CTRL_SELECT, 0, CELL_PAD_CTRL_START, CELL_PAD_CTRL_L3, CELL_PAD_CTRL_R3,
        CELL_PAD_CTRL_L1, CELL_PAD_CTRL_R1, CELL_PAD_CTRL_UP, CELL_PAD_CTRL_DOWN,
        CELL_PAD_CTRL_LEFT, CELL_PAD_CTRL_RIGHT
    };
    for (unsigned b = 0; b < sizeof buttons / sizeof buttons[0]; ++b) {
        if (!buttons[b]) continue;
        assert(SDL_JoystickSetVirtualButton(ps5, b, 1) == 0);
        wait_state(0, 1, buttons[b]);
        assert((read_word(CELL_PAD_BTN_OFFSET_DIGITAL1) |
               (read_word(CELL_PAD_BTN_OFFSET_DIGITAL2) << 8)) == buttons[b]);
        assert(SDL_JoystickSetVirtualButton(ps5, b, 0) == 0);
        wait_state(0, 1, 0);
    }
    SDL_JoystickSetVirtualButton(ps5, 20, 1);
    wait_state(0, 1, CELL_PAD_CTRL_SELECT);
    SDL_JoystickSetVirtualButton(ps5, 20, 0);
    SDL_JoystickSetVirtualAxis(ps5, 0, -32768);
    SDL_JoystickSetVirtualAxis(ps5, 1, 32767);
    SDL_JoystickSetVirtualAxis(ps5, 2, 32767);
    SDL_JoystickSetVirtualAxis(ps5, 3, -32768);
    SDL_JoystickSetVirtualAxis(ps5, 4, 0); /* half pull */
    SDL_JoystickSetVirtualAxis(ps5, 5, 32767);
    wait_state(0, 1, CELL_PAD_CTRL_L2 | CELL_PAD_CTRL_R2);
    assert(read_word(CELL_PAD_BTN_OFFSET_ANALOG_LEFT_X) == 0);
    assert(read_word(CELL_PAD_BTN_OFFSET_ANALOG_LEFT_Y) == 255);
    assert(read_word(CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_X) == 255);
    assert(read_word(CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_Y) == 0);
    u16 pressure = read_word(CELL_PAD_BTN_OFFSET_PRESS_L2);
    assert(pressure >= 126 && pressure <= 128);
    assert(read_word(CELL_PAD_BTN_OFFSET_PRESS_R2) == 255);

    guest[0x400] = 1; guest[0x401] = 128;
    assert(cellPadSetActDirect(0, (CellPadActParam*)(uintptr_t)0x400) == CELL_OK);
    for (int n = 0; n < 500 && SDL_AtomicGet(&rumble_high) != 65535; ++n) SDL_Delay(4);
    assert(SDL_AtomicGet(&rumble_low) == 128 * 257);
    assert(SDL_AtomicGet(&rumble_high) == 65535);
    /* A stalled guest must not leave the motors running indefinitely. */
    for (int n = 0; n < 500 && SDL_AtomicGet(&rumble_high); ++n) SDL_Delay(4);
    assert(SDL_AtomicGet(&rumble_low) == 0 && SDL_AtomicGet(&rumble_high) == 0);

    SDL_Joystick* nintendo = attach("Switch Pro Controller", 0x057e, 0x2009, 1);
    SDL_Joystick* xbox = attach("Xbox Series Controller", 0x045e, 0x0b13, 1);
    wait_state(1, 1, 0);
    wait_state(2, 1, 0);
    detach(ps5);
    wait_state(0, 0, 0);
    assert(s_host_state[0].trigger_l2 == 0 && s_host_state[0].analog_lx == 128);
    /* Removing an earlier SDL device cannot move surviving controller slots. */
    SDL_JoystickSetVirtualButton(nintendo, 0, 1);
    SDL_JoystickSetVirtualButton(xbox, 1, 1);
    wait_state(1, 1, CELL_PAD_CTRL_CROSS);
    wait_state(2, 1, CELL_PAD_CTRL_CIRCLE);
    ps5 = attach("DualSense", 0x054c, 0x0ce6, 1);
    wait_state(0, 1, 0);
    SDL_LockMutex(port_pad_lock);
    assert(port_pad_rumble[0].lo == 0 && port_pad_rumble[0].hi == 0);
    SDL_UnlockMutex(port_pad_lock);
    /* The callback for the unplugged virtual device cannot receive motor-off.
     * Observe fresh callbacks from the newly connected instance. */
    SDL_AtomicSet(&rumble_low, 0);
    SDL_AtomicSet(&rumble_high, 0);
    /* Explicit motor-off requests must take effect before the timeout. */
    assert(cellPadSetActDirect(0, (CellPadActParam*)(uintptr_t)0x400) == CELL_OK);
    for (int n = 0; n < 500 && SDL_AtomicGet(&rumble_high) != 65535; ++n) SDL_Delay(4);
    assert(SDL_AtomicGet(&rumble_high) == 65535);
    guest[0x400] = guest[0x401] = 0;
    assert(cellPadSetActDirect(0, (CellPadActParam*)(uintptr_t)0x400) == CELL_OK);
    for (int n = 0; n < 500 && SDL_AtomicGet(&rumble_high); ++n) SDL_Delay(4);
    assert(SDL_AtomicGet(&rumble_low) == 0 && SDL_AtomicGet(&rumble_high) == 0);
    /* Shutdown must stop an actively rumbling, still attached device. */
    guest[0x400] = 1; guest[0x401] = 255;
    assert(cellPadSetActDirect(0, (CellPadActParam*)(uintptr_t)0x400) == CELL_OK);
    for (int n = 0; n < 500 && SDL_AtomicGet(&rumble_high) != 65535; ++n) SDL_Delay(4);
    assert(SDL_AtomicGet(&rumble_high) == 65535);
    assert(cellPadEnd() == CELL_OK);
    assert(SDL_AtomicGet(&rumble_low) == 0 && SDL_AtomicGet(&rumble_high) == 0);
    assert(cellPadInit(4) == CELL_OK);
    /* A new API session assigns ports in the current enumeration order. */
    wait_state(0, 1, CELL_PAD_CTRL_CROSS);
    detach(ps5); detach(nintendo); detach(xbox); detach(flight);
    wait_state(0, 0, 0);
    assert(cellPadEnd() == CELL_OK);
    assert(!port_pad_thread && !port_pad_lock);
    assert(cellPadInit(1) == CELL_OK);
    assert(read_word(CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_Y) == 128);
    assert(cellPadEnd() == CELL_OK);
    SDL_QuitSubSystem(SDL_INIT_JOYSTICK);
    puts("gamepad contract passed");
    return 0;
}
