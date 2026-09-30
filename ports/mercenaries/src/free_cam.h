#ifndef MERCENARIES_FREE_CAM_H
#define MERCENARIES_FREE_CAM_H
#include <stdint.h>
#include "xinput_xbox.h"
void recomp_freecam_set(int enabled);
void recomp_freecam_time_key(unsigned key);
uint32_t recomp_freecam_game_ticks(uint32_t ticks);
float recomp_freecam_time_scale(void);
int recomp_freecam_enabled(void);
void recomp_freecam_context(uint32_t main_state);
void recomp_freecam_input(XBOX_INPUT_STATE *state,int gameplay,int blocked);
void recomp_freecam_camera(uint32_t camera,uint32_t matrix,float dt);
int recomp_freecam_focus(float position_out[3],float direction_out[3]);
#endif
