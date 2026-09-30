#ifndef XBOXRECOMP_DUALSENSE_SDL_H
#define XBOXRECOMP_DUALSENSE_SDL_H

#include "xinput_xbox.h"

#if defined(_WIN32)
void dualsense_sdl_init(const BOOL xinput_slots[XBOX_MAX_CONTROLLERS]);
DWORD dualsense_sdl_get_state(DWORD port, XBOX_INPUT_STATE *state);
DWORD dualsense_sdl_set_vibration(DWORD port, const XBOX_VIBRATION *vibration);
BOOL dualsense_sdl_is_connected(DWORD port);
#endif

#endif