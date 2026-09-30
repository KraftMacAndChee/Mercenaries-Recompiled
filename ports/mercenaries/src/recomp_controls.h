#ifndef MERCENARIES_CONTROLS_H
#define MERCENARIES_CONTROLS_H
#include <windows.h>
#include <stdint.h>
#include "xinput_xbox.h"
void recomp_controls_freecam_input(XBOX_INPUT_STATE *state);
void recomp_controls_freecam_look(float *x,float *y);
float recomp_controls_freecam_roll(void);
int recomp_controls_freecam_menu_key(unsigned key);
#define RECOMP_CONTROLS_TITLE 0x52440000u
#define RECOMP_CONTROLS_ROW 0x52440001u
#define RECOMP_CONTROLS_ROWS 11
void recomp_controls_init(HWND window);
int recomp_controls_hide_hud_brush(uint32_t brush,uint32_t canvas);
void recomp_controls_waiting_controller(int waiting);
void recomp_controls_substate(uint32_t substate);
int recomp_controls_event_current(uint32_t main,uint32_t sub,uint32_t joystick,uint32_t joysub);
int recomp_controls_aim_assist(void);
int recomp_controls_mouse_look_pending(void);
void recomp_controls_state(uint32_t main, uint32_t sub, uint32_t joystick, int enabled);
int recomp_controls_message(HWND hwnd, UINT message, WPARAM wp, LPARAM lp);
void recomp_controls_open(void);
void recomp_controls_close(void);
const char *recomp_controls_label(uint32_t hash);
int recomp_controls_row_binding(uint32_t hash,int *keyboard,unsigned short *binding);
/* Return 2 to ask the retail handler to close, 1 to consume, 0 to navigate. */
uint32_t recomp_controls_input(unsigned row, unsigned input, unsigned event);
unsigned recomp_controls_count(void);
int recomp_controls_is_binding(void);
void recomp_controls_prompt_gameplay_body(int enabled);
int recomp_controls_prompt_action_available(unsigned action,int device);
unsigned short recomp_controls_prompt_binding(unsigned action,int device,int context);
unsigned short recomp_controls_prompt_support_binding(int device);
#endif
