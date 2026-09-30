/* Optional native PlayStation controller backend for Windows.
 *
 * XInput remains available for Xbox pads. SDL2 is loaded at runtime and only
 * Sony PS3/PS4/PS5 controllers are accepted; the first one owns player one so a
 * stale virtual XInput slot cannot hide it. This lets a DualSense work without
 * DS4Windows while keeping SDL optional. */
#include "dualsense_sdl.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdint.h>

#include <string.h>

typedef struct SDL_GameController SDL_GameController;
typedef int16_t SDL_JoystickAxis;
typedef int SDL_bool;

enum {
    SDL_INIT_GAMECONTROLLER_VALUE = 0x00002000u,
    SDL_CONTROLLER_TYPE_PS3_VALUE = 3,
    SDL_CONTROLLER_TYPE_PS4_VALUE = 4,
    SDL_CONTROLLER_TYPE_PS5_VALUE = 7,
    SDL_AXIS_LEFTX = 0,
    SDL_AXIS_LEFTY = 1,
    SDL_AXIS_RIGHTX = 2,
    SDL_AXIS_RIGHTY = 3,
    SDL_AXIS_TRIGGERLEFT = 4,
    SDL_AXIS_TRIGGERRIGHT = 5,
    SDL_BUTTON_A = 0,
    SDL_BUTTON_B = 1,
    SDL_BUTTON_X = 2,
    SDL_BUTTON_Y = 3,
    SDL_BUTTON_BACK = 4,
    SDL_BUTTON_START = 6,
    SDL_BUTTON_LEFTSTICK = 7,
    SDL_BUTTON_RIGHTSTICK = 8,
    SDL_BUTTON_LEFTSHOULDER = 9,
    SDL_BUTTON_RIGHTSHOULDER = 10,
    SDL_BUTTON_DPAD_UP = 11,
    SDL_BUTTON_DPAD_DOWN = 12,
    SDL_BUTTON_DPAD_LEFT = 13,
    SDL_BUTTON_DPAD_RIGHT = 14
};

typedef uint32_t (__cdecl *sdl_was_init_fn)(uint32_t);
typedef int (__cdecl *sdl_init_subsystem_fn)(uint32_t);
typedef int (__cdecl *sdl_num_joysticks_fn)(void);
typedef SDL_bool (__cdecl *sdl_is_game_controller_fn)(int);
typedef SDL_GameController *(__cdecl *sdl_open_fn)(int);
typedef void (__cdecl *sdl_close_fn)(SDL_GameController *);
typedef int (__cdecl *sdl_type_fn)(SDL_GameController *);
typedef SDL_bool (__cdecl *sdl_attached_fn)(SDL_GameController *);
typedef void (__cdecl *sdl_update_fn)(void);
typedef uint8_t (__cdecl *sdl_button_fn)(SDL_GameController *, int);
typedef SDL_JoystickAxis (__cdecl *sdl_axis_fn)(SDL_GameController *, int);
typedef int (__cdecl *sdl_rumble_fn)(SDL_GameController *, uint16_t,
                                     uint16_t, uint32_t);

typedef struct SDLControllerAPI {
    HMODULE module;
    sdl_was_init_fn was_init;
    sdl_init_subsystem_fn init_subsystem;
    sdl_num_joysticks_fn num_joysticks;
    sdl_is_game_controller_fn is_game_controller;
    sdl_open_fn open;
    sdl_close_fn close;
    sdl_type_fn get_type;
    sdl_attached_fn get_attached;
    sdl_update_fn update;
    sdl_button_fn get_button;
    sdl_axis_fn get_axis;
    sdl_rumble_fn rumble;
} SDLControllerAPI;

static SDLControllerAPI g_sdl;
static SDL_GameController *g_pads[XBOX_MAX_CONTROLLERS];
static DWORD g_packets[XBOX_MAX_CONTROLLERS];
static ULONGLONG g_last_rescan_ms;

#define LOAD_SDL(api, symbol, type) \
    (api) = (type)GetProcAddress(g_sdl.module, (symbol))

static BOOL load_api(void)
{
    if (g_sdl.module)
        return TRUE;
    g_sdl.module = LoadLibraryA("SDL2.dll");
    if (!g_sdl.module)
        return FALSE;
    LOAD_SDL(g_sdl.was_init, "SDL_WasInit", sdl_was_init_fn);
    LOAD_SDL(g_sdl.init_subsystem, "SDL_InitSubSystem", sdl_init_subsystem_fn);
    LOAD_SDL(g_sdl.num_joysticks, "SDL_NumJoysticks", sdl_num_joysticks_fn);
    LOAD_SDL(g_sdl.is_game_controller, "SDL_IsGameController", sdl_is_game_controller_fn);
    LOAD_SDL(g_sdl.open, "SDL_GameControllerOpen", sdl_open_fn);
    LOAD_SDL(g_sdl.close, "SDL_GameControllerClose", sdl_close_fn);
    LOAD_SDL(g_sdl.get_type, "SDL_GameControllerGetType", sdl_type_fn);
    LOAD_SDL(g_sdl.get_attached, "SDL_GameControllerGetAttached", sdl_attached_fn);
    LOAD_SDL(g_sdl.update, "SDL_GameControllerUpdate", sdl_update_fn);
    LOAD_SDL(g_sdl.get_button, "SDL_GameControllerGetButton", sdl_button_fn);
    LOAD_SDL(g_sdl.get_axis, "SDL_GameControllerGetAxis", sdl_axis_fn);
    LOAD_SDL(g_sdl.rumble, "SDL_GameControllerRumble", sdl_rumble_fn);
    if (!g_sdl.was_init || !g_sdl.init_subsystem || !g_sdl.num_joysticks ||
        !g_sdl.is_game_controller || !g_sdl.open || !g_sdl.close ||
        !g_sdl.get_type || !g_sdl.get_attached || !g_sdl.update ||
        !g_sdl.get_button || !g_sdl.get_axis) {
        FreeLibrary(g_sdl.module);
        memset(&g_sdl, 0, sizeof(g_sdl));
        return FALSE;
    }
    return TRUE;
}

static void open_first_playstation_controller(void)
{
    int count;
    int device;

    if (g_pads[0] && g_sdl.get_attached(g_pads[0]))
        return;
    if (g_pads[0]) {
        g_sdl.close(g_pads[0]);
        g_pads[0] = NULL;
    }
    count = g_sdl.num_joysticks();
    for (device = 0; device < count; ++device) {
        SDL_GameController *controller;
        int type;
        if (!g_sdl.is_game_controller(device))
            continue;
        controller = g_sdl.open(device);
        if (!controller)
            continue;
        type = g_sdl.get_type(controller);
        if (type == SDL_CONTROLLER_TYPE_PS3_VALUE ||
            type == SDL_CONTROLLER_TYPE_PS4_VALUE ||
            type == SDL_CONTROLLER_TYPE_PS5_VALUE) {
            g_pads[0] = controller;
            return;
        }
        g_sdl.close(controller);
    }
}

void dualsense_sdl_init(const BOOL xinput_slots[XBOX_MAX_CONTROLLERS])
{
    (void)xinput_slots;
    if (!load_api())
        return;
    if ((g_sdl.was_init(SDL_INIT_GAMECONTROLLER_VALUE) &
         SDL_INIT_GAMECONTROLLER_VALUE) == 0u &&
        g_sdl.init_subsystem(SDL_INIT_GAMECONTROLLER_VALUE) != 0)
        return;
    open_first_playstation_controller();
    g_last_rescan_ms = GetTickCount64();
}

static BYTE trigger_to_byte(SDL_JoystickAxis value)
{
    int normalized = (int)value;
    if (normalized < 0)
        normalized = 0;
    return (BYTE)(normalized >> 7);
}

DWORD dualsense_sdl_get_state(DWORD port, XBOX_INPUT_STATE *state)
{
    SDL_GameController *controller;
    WORD buttons = 0;
    if (port >= XBOX_MAX_CONTROLLERS || !state || !g_sdl.module)
        return ERROR_DEVICE_NOT_CONNECTED;
    if (port != 0u)
        return ERROR_DEVICE_NOT_CONNECTED;
    controller = g_pads[0];
    if (!controller || !g_sdl.get_attached(controller)) {
        const ULONGLONG now = GetTickCount64();
        if (now - g_last_rescan_ms >= 1000u) {
            open_first_playstation_controller();
            g_last_rescan_ms = now;
            controller = g_pads[0];
        }
        if (!controller || !g_sdl.get_attached(controller))
            return ERROR_DEVICE_NOT_CONNECTED;
    }
    g_sdl.update();
    memset(state, 0, sizeof(*state));
    state->dwPacketNumber = ++g_packets[port];
    if (g_sdl.get_button(controller, SDL_BUTTON_DPAD_UP)) buttons |= XBOX_GAMEPAD_DPAD_UP;
    if (g_sdl.get_button(controller, SDL_BUTTON_DPAD_DOWN)) buttons |= XBOX_GAMEPAD_DPAD_DOWN;
    if (g_sdl.get_button(controller, SDL_BUTTON_DPAD_LEFT)) buttons |= XBOX_GAMEPAD_DPAD_LEFT;
    if (g_sdl.get_button(controller, SDL_BUTTON_DPAD_RIGHT)) buttons |= XBOX_GAMEPAD_DPAD_RIGHT;
    if (g_sdl.get_button(controller, SDL_BUTTON_START)) buttons |= XBOX_GAMEPAD_START;
    if (g_sdl.get_button(controller, SDL_BUTTON_BACK)) buttons |= XBOX_GAMEPAD_BACK;
    if (g_sdl.get_button(controller, SDL_BUTTON_LEFTSTICK)) buttons |= XBOX_GAMEPAD_LEFT_THUMB;
    if (g_sdl.get_button(controller, SDL_BUTTON_RIGHTSTICK)) buttons |= XBOX_GAMEPAD_RIGHT_THUMB;
    state->Gamepad.wButtons = buttons;
    state->Gamepad.bAnalogButtons[XBOX_BUTTON_A] = g_sdl.get_button(controller, SDL_BUTTON_A) ? 255 : 0;
    state->Gamepad.bAnalogButtons[XBOX_BUTTON_B] = g_sdl.get_button(controller, SDL_BUTTON_B) ? 255 : 0;
    state->Gamepad.bAnalogButtons[XBOX_BUTTON_X] = g_sdl.get_button(controller, SDL_BUTTON_X) ? 255 : 0;
    state->Gamepad.bAnalogButtons[XBOX_BUTTON_Y] = g_sdl.get_button(controller, SDL_BUTTON_Y) ? 255 : 0;
    state->Gamepad.bAnalogButtons[XBOX_BUTTON_BLACK] = g_sdl.get_button(controller, SDL_BUTTON_RIGHTSHOULDER) ? 255 : 0;
    state->Gamepad.bAnalogButtons[XBOX_BUTTON_WHITE] = g_sdl.get_button(controller, SDL_BUTTON_LEFTSHOULDER) ? 255 : 0;
    state->Gamepad.bAnalogButtons[XBOX_BUTTON_LTRIGGER] = trigger_to_byte(g_sdl.get_axis(controller, SDL_AXIS_TRIGGERLEFT));
    state->Gamepad.bAnalogButtons[XBOX_BUTTON_RTRIGGER] = trigger_to_byte(g_sdl.get_axis(controller, SDL_AXIS_TRIGGERRIGHT));
    state->Gamepad.sThumbLX = g_sdl.get_axis(controller, SDL_AXIS_LEFTX);
    state->Gamepad.sThumbLY = (SHORT)(-1 - g_sdl.get_axis(controller, SDL_AXIS_LEFTY));
    state->Gamepad.sThumbRX = g_sdl.get_axis(controller, SDL_AXIS_RIGHTX);
    state->Gamepad.sThumbRY = (SHORT)(-1 - g_sdl.get_axis(controller, SDL_AXIS_RIGHTY));
    return ERROR_SUCCESS;
}

DWORD dualsense_sdl_set_vibration(DWORD port,
                                  const XBOX_VIBRATION *vibration)
{
    SDL_GameController *controller;
    if (port >= XBOX_MAX_CONTROLLERS || !vibration || !g_sdl.module)
        return ERROR_DEVICE_NOT_CONNECTED;
    controller = g_pads[port];
    if (!controller || !g_sdl.get_attached(controller))
        return ERROR_DEVICE_NOT_CONNECTED;
    if (!g_sdl.rumble)
        return ERROR_SUCCESS;
    return g_sdl.rumble(controller, vibration->wLeftMotorSpeed,
                        vibration->wRightMotorSpeed, 1000u) == 0
        ? ERROR_SUCCESS : ERROR_DEVICE_NOT_CONNECTED;
}

BOOL dualsense_sdl_is_connected(DWORD port)
{
    return port < XBOX_MAX_CONTROLLERS && g_sdl.module && g_pads[port] &&
           g_sdl.get_attached(g_pads[port]);
}
#endif