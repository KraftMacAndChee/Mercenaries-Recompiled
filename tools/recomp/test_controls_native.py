"""Exercise the production controls module with synthetic device/OS input.
No real keyboard/controller is polled, and no cursor or user settings move.
"""
from pathlib import Path
import subprocess,tempfile
ROOT=Path(__file__).resolve().parents[2]
source=r"""
#include <windows.h>
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "xinput_xbox.h"
static HWND test_focus=(HWND)1;
static int test_keys[256],test_overlay,test_clip,test_cursor,test_is_iconic;
static char test_ini[MAX_PATH];
static XBOX_INPUT_MAPPER registered_mapper;
static SHORT test_key(int key){return test_keys[key]?(SHORT)0x8000:0;}
static unsigned test_overlay_consumed;
BOOL xbox_InputKeyDown(int key){return key>0 && key<256 && (unsigned)key!=test_overlay_consumed && test_keys[key];}
BOOL xbox_InputKeyDownContinuous(int key){return key>0 && key<256 && !test_overlay && test_keys[key];}
static HWND test_foreground(void){return test_focus;}
static BOOL test_iconic(HWND h){return test_is_iconic;}
static BOOL test_rect(HWND h,RECT*r){r->left=0;r->top=0;r->right=1280;r->bottom=720;return TRUE;}
static BOOL test_screen(HWND h,POINT*p){p->x+=100;p->y+=50;return TRUE;}
static BOOL test_getpos(POINT*p){p->x=500;p->y=400;return TRUE;}
static BOOL test_setpos(int x,int y){return TRUE;}
static BOOL test_cliprect(const RECT*r){test_clip=r!=NULL;return TRUE;}
static HCURSOR test_setcursor(HCURSOR c){test_cursor=c!=NULL;return c;}
static UINT_PTR test_timer(HWND h,UINT_PTR id,UINT delay,TIMERPROC proc){return id;}
static BOOL test_raw(PCRAWINPUTDEVICE d,UINT n,UINT sz){return TRUE;}
static DWORD test_module(HMODULE m,LPSTR text,DWORD size){strncpy(text,test_ini,size);return (DWORD)strlen(text);}
void xbox_InputSetMapper(XBOX_INPUT_MAPPER m){registered_mapper=m;}
static unsigned test_prompt_keyboard;
void xbox_InputNotifyKeyboardActivity(void){++test_prompt_keyboard;}
static int test_developer_allowed=1;
int recomp_dev_menu_allowed(void){return test_developer_allowed;}
int recomp_dev_menu_visible(void){return test_overlay;}
static int test_overlay_camera,test_text_entry,test_overlay_pad;
int recomp_freecam_enabled(void);
int recomp_dev_menu_camera_input(int keyboard){
 return test_overlay && test_overlay_camera && recomp_freecam_enabled() && test_focus && !test_is_iconic && (!keyboard || !test_text_entry);
}
DWORD xbox_InputGetOverlayState(DWORD port,XBOX_INPUT_STATE *state){
 memset(state,0,sizeof(*state));
 if(test_overlay_pad)state->Gamepad.sThumbLY=32767;
 registered_mapper(state,!test_overlay_pad);return ERROR_SUCCESS;
}
static int test_azerty;
static UINT test_mapkey(UINT key,UINT mode){
 static const unsigned scans[]={0x31,0x32,0x2c,0x2d,0x2e,0x2f,0x10,0x12,0x1e,0x20,0x1f,0x11,0x24,0x26,0x25,0x17};
 static const char values[]="N M Z X C V Q E A D S W J L K I";
 for(unsigned i=0;i<16;++i)if(key==scans[i]){
 unsigned v=values[i*2];if(test_azerty){if(v=='W')v='Z';else if(v=='Z')v='W';else if(v=='A')v='Q';else if(v=='Q')v='A';}return v;}return 0;
}
#define MapVirtualKeyW test_mapkey
#define GetTickCount64() ((ULONGLONG)GetTickCount())
#define GetAsyncKeyState test_key
#define GetForegroundWindow test_foreground
#define IsIconic test_iconic
#define GetClientRect test_rect
#define ClientToScreen test_screen
#define GetCursorPos test_getpos
#define SetCursorPos test_setpos
#define ClipCursor test_cliprect
#define SetCursor test_setcursor
#define SetTimer test_timer
#define RegisterRawInputDevices test_raw
#define GetModuleFileNameA test_module
#include "recomp_controls.c"
#include "free_cam.c"
/*DEVELOPER_HOTKEY*/
ptrdiff_t g_xbox_mem_offset;
static void reset_input(void){memset(g_capture_held,0,sizeof(g_capture_held));memset(g_satellite_back_active,0,sizeof(g_satellite_back_active));memset(g_menu_held,0,sizeof(g_menu_held));memset(test_keys,0,sizeof(test_keys));g_listen=-1;g_release=0;g_armed=0;}
static void play(unsigned joy){g_menu=0;test_focus=(HWND)1;test_overlay=0;recomp_controls_state(0x4249D707,0xC2CBD863,joy,1);}
static unsigned char camera_memory[0x420000];
static void camera_matrix(unsigned m){
 memset(camera_memory,0,sizeof(camera_memory));
 MEM32(0x10024)=0x12000;
 MEM32(0x413F6C)=0x4249D707;MEMF(0x413F98)=.1f;
 MEMF(m)=MEMF(m+0x14)=MEMF(m+0x28)=MEMF(m+0x3c)=1;
 MEMF(m+0x30)=10;MEMF(m+0x34)=20;MEMF(m+0x38)=30;
}
static void test_free_camera(void){
 g_xbox_mem_offset=(ptrdiff_t)camera_memory-0x10000;
 const unsigned m=0x11000;XBOX_INPUT_STATE pad={0};
 reset_input();play(4);g_test_dx=g_test_dy=g_dx=g_dy=0;
 camera_matrix(m);recomp_freecam_set(0);test_developer_allowed=0;
 test_developer_hotkey(VK_F11,0);assert(!recomp_freecam_enabled());
 test_developer_allowed=1;
 assert(!recomp_controls_message((HWND)1,WM_KEYDOWN,VK_F11,0));
 test_developer_hotkey(VK_F11,0);assert(recomp_freecam_enabled());
 test_developer_hotkey(VK_F11,1L<<30);assert(recomp_freecam_enabled());
 recomp_freecam_time_key('2');test_developer_hotkey(VK_F11,0);
 assert(!recomp_freecam_enabled() && recomp_freecam_time_scale()==1);
 test_developer_hotkey(VK_F11,0);recomp_freecam_time_key('1');
 test_developer_hotkey(VK_F11,0);assert(recomp_freecam_time_scale()==1);
 for(int hz=30;hz<=60;hz+=30){
  recomp_freecam_set(0);camera_matrix(m);recomp_freecam_set(1);
  for(int i=0;i<hz;++i){
   pad=(XBOX_INPUT_STATE){0};pad.Gamepad.sThumbLY=32767;pad.Gamepad.wButtons=0x10;
   recomp_freecam_input(&pad,1,0);assert(pad.Gamepad.sThumbLY==0 && pad.Gamepad.wButtons==0x10);
   MEMF(0x413F98)=1.f/hz;recomp_freecam_camera(0x10000,m,1.f/hz);
  }
  assert(fabsf(MEMF(m+0x38)-10)<.001f);assert(MEMF(m+0x30)==10 && MEMF(m+0x34)==20);
 }

 /* Frozen simulation does not freeze the camera or overwrite authored rates. */
 MEMF(0x4140D4)=.7f;MEMF(0x413F98)=.1f;
 recomp_freecam_time_key('2');assert(fabsf(recomp_freecam_time_scale()-.2f)<.0001f);
 unsigned ticks=0;for(unsigned i=0;i<100;++i)ticks+=recomp_freecam_game_ticks(1);assert(ticks==20);
 recomp_freecam_time_key('1');assert(recomp_freecam_time_scale()==0 && recomp_freecam_game_ticks(100)==0);
 float before=MEMF(m+0x38);pad.Gamepad.sThumbLY=32767;
 recomp_freecam_input(&pad,1,0);recomp_freecam_camera(0x10000,m,0);assert(MEMF(m+0x38)<before);
 recomp_freecam_time_key('1');assert(fabsf(recomp_freecam_time_scale()-.2f)<.0001f);
 recomp_freecam_time_key('2');assert(recomp_freecam_time_scale()==1 && recomp_freecam_game_ticks(100)==100);
 recomp_freecam_time_key('1');recomp_freecam_set(0);assert(recomp_freecam_time_scale()==1);
 assert(fabsf(MEMF(0x4140D4)-.7f)<.00001f); /* developer mode never rewrites the retail rate */
 recomp_freecam_set(1);MEMF(m+0x38)=10;
 /* Autorepeat toggles once; a custom 1/2 movement/pause binding cannot leak. */
 controls_assign(g_binds[0][3],19,'2');test_keys['2']=1;XBOX_INPUT_STATE mapped={0};mapper(&mapped,TRUE);
 assert(mapped.Gamepad.sThumbLY==0);
 assert(recomp_controls_message((HWND)1,WM_KEYDOWN,'2',0));
 assert(recomp_freecam_time_scale()<1);recomp_controls_message((HWND)1,WM_KEYDOWN,'2',1L<<30);
 assert(recomp_freecam_time_scale()<1);recomp_controls_message((HWND)1,WM_KEYDOWN,'2',0);
 assert(recomp_freecam_time_scale()==1);test_keys['2']=0;
 /* Pause and the dev menu discard cached movement. */
 pad.Gamepad.sThumbLY=32767;recomp_freecam_input(&pad,1,1);recomp_freecam_camera(0x10000,m,.1f);
 assert(fabsf(MEMF(m+0x38)-10)<.001f && pad.Gamepad.sThumbLY==0);
 recomp_freecam_input(&pad,0,0);recomp_freecam_camera(0x10000,m,.1f);assert(fabsf(MEMF(m+0x38)-10)<.001f);
 /* Disabling leaves the next retail camera matrix and input untouched. */
 recomp_freecam_set(0);camera_matrix(m);pad.Gamepad.sThumbLY=12345;
 recomp_freecam_input(&pad,1,0);assert(pad.Gamepad.sThumbLY==12345);
 recomp_freecam_camera(0x10000,m,.1f);assert(MEMF(m+0x38)==30);
 /* A new session starts from the current pose, and cutscenes own their camera. */
 MEMF(m+0x30)=100;recomp_freecam_set(1);pad=(XBOX_INPUT_STATE){0};recomp_freecam_input(&pad,1,0);
 recomp_freecam_camera(0x10000,m,.016f);assert(MEMF(m+0x30)==100);
 MEM32(0x10024)=0x13000;MEMF(m+0x30)=200;
 recomp_freecam_camera(0x10000,m,.016f);assert(!recomp_freecam_enabled() && MEMF(m+0x30)==200);
 recomp_freecam_set(1);recomp_freecam_context(0x59C00449);assert(!recomp_freecam_enabled());
 g_xbox_mem_offset=(ptrdiff_t)camera_memory-0x10000;
}

static void test_free_camera_roll(void){
 const unsigned m=0x11000;XBOX_INPUT_STATE pad={0};
 reset_input();play(4);g_test_dx=g_test_dy=g_dx=g_dy=0;
 for(int direction=-1;direction<=1;direction+=2)for(int hz=30;hz<=144;hz+=(hz==30?30:84)){
  recomp_freecam_set(0);camera_matrix(m);recomp_freecam_set(1);
  test_keys[direction>0?'Q':'E']=1;MEMF(0x413F98)=1.f/hz;
  for(int i=0;i<hz;i++)recomp_freecam_camera(0x10000,m,0);
  assert(fabsf(MEMF(m)-.5f)<.0001f);
  assert(fabsf(MEMF(m+4)-direction*.8660254f)<.0001f);
  assert(fabsf(MEMF(m+0x10)+direction*.8660254f)<.0001f);
  assert(fabsf(MEMF(m+0x14)-.5f)<.0001f);
  assert(MEMF(m+0x28)==1 && MEMF(m+0x30)==10 && MEMF(m+0x34)==20 && MEMF(m+0x38)==30);
  float pos[3],dir[3];assert(recomp_freecam_focus(pos,dir) && dir[2]==-1.f);
  test_keys['Q']=test_keys['E']=0;
  float bank=MEMF(m+4);recomp_freecam_camera(0x10000,m,0);assert(MEMF(m+4)==bank);
  test_keys['Q']=test_keys['E']=1;recomp_freecam_camera(0x10000,m,0);assert(MEMF(m+4)==bank);
  test_keys['Q']=test_keys['E']=0;
 }
 recomp_freecam_set(0);camera_matrix(m);recomp_freecam_set(1);
 recomp_freecam_camera(0x10000,m,0);assert(MEMF(m+4)==0); /* New session levels the camera. */
 test_keys['Q']=1;recomp_freecam_time_key('1');recomp_freecam_camera(0x10000,m,0);
 assert(MEMF(m+4)>0);float bank=MEMF(m+4);
 test_focus=NULL;recomp_freecam_camera(0x10000,m,0);assert(MEMF(m+4)==bank);test_focus=(HWND)1;
 test_overlay=1;recomp_freecam_camera(0x10000,m,0);assert(MEMF(m+4)==bank);test_overlay=0;
 g_modal=1;recomp_freecam_camera(0x10000,m,0);assert(MEMF(m+4)==bank);g_modal=0;
 g_menu=1;recomp_freecam_camera(0x10000,m,0);assert(MEMF(m+4)==bank);g_menu=0;
 g_gameplay=0;recomp_freecam_camera(0x10000,m,0);assert(MEMF(m+4)==bank);play(4);
 /* Reserved keys cannot also move the camera through custom gameplay binds. */
 unsigned short old_q=g_binds[0][3][19],old_e=g_binds[0][3][18];
 g_binds[0][3][19]='Q';g_binds[0][3][18]='E';
 mapper(&pad,TRUE);assert(pad.Gamepad.sThumbLY==0);
 recomp_freecam_set(0);mapper(&pad,TRUE);assert(pad.Gamepad.sThumbLY==32767);
 test_keys['Q']=0;test_keys['E']=1;recomp_freecam_set(1);mapper(&pad,TRUE);assert(pad.Gamepad.sThumbLY==0);
 recomp_freecam_set(0);mapper(&pad,TRUE);assert(pad.Gamepad.sThumbLY==-32767);
 g_binds[0][3][19]=old_q;g_binds[0][3][18]=old_e;reset_input();
}

static void test_vehicle_contexts(void){
 XBOX_INPUT_STATE state;
 /* Exact usable retail channels: 22 helicopter / 18 car / 18 tank actions.
  * Check every physical source, including sources whose original slot was
  * removed (e.g. left-stick-forward can now bind the car accelerator). */
 const unsigned active[]={0xffff3f,0xf3ff3c,0xfffa3c};
 for(unsigned c=0;c<3;++c){
  g_edit_context=(int)c;g_page=0;
  unsigned listed=0,count=0;
  for(unsigned page=0;page<page_count();++page){
   g_page=page;assert(visible_actions()>0 && visible_actions()<=4);
   for(unsigned row=0;row<visible_actions();++row){
    unsigned a=action_at(row);assert(a<24 && !(listed&(1u<<a)));listed|=1u<<a;++count;
    assert(recomp_controls_label(RECOMP_CONTROLS_ROW+row+2));
   }
  }
  assert(listed==active[c] && count==(c==0?22:18));
  for(int d=0;d<2;++d)for(unsigned a=0;a<26;++a)g_binds[d][c][a]=legacy_default_binding(d,c,a);
  for(int d=0;d<2;++d)for(unsigned a=0;a<24;++a){
   reset_input();play(c+1);state=(XBOX_INPUT_STATE){0};
   if(!d)test_keys[keys[a]]=1;
   else{float input[24]={0};input[a]=1;pack(&state.Gamepad,input);}
   assert(mapper(&state,!d));float result[24];unpack(&state.Gamepad,result);
   for(unsigned b=0;b<24;++b)assert((result[b]>.5f)==(a==b && (active[c]&(1u<<a))));
   assert(recomp_controls_prompt_binding(a,d?XBOX_PROMPT_PLAYSTATION:XBOX_PROMPT_KEYBOARD,-1)==
    ((active[c]&(1u<<a))?(d?a:keys[a]):0xffffu));
  }
  for(int d=0;d<2;++d)for(unsigned source=0;source<24;++source){
   for(unsigned a=0;a<26;++a)g_binds[d][c][a]=legacy_default_binding(d,c,a);
   reset_input();play(c+1);g_edit_context=c;g_device=d;
   unsigned action=c==1?8:15;g_listen=action;accept_binding(d?source:keys[source]);g_release=0;
   state=(XBOX_INPUT_STATE){0};
   if(!d)test_keys[keys[source]]=1;
   else{float input[24]={0};input[source]=.75f;pack(&state.Gamepad,input);}
   assert(mapper(&state,!d));float result[24];unpack(&state.Gamepad,result);
   for(unsigned b=0;b<24;++b)assert((result[b]>.5f)==(b==action || (b==source && (active[c]&(1u<<source)))));
  }
 }
 /* Conflicting legacy per-mode maps must never override the shared owner,
  * on either device, even after an INI reload. */
 for(int d=0;d<2;++d)for(int c=0;c<CONTROL_CONTEXTS;++c){
  g_device=d;g_edit_context=c;
  for(unsigned a=0;a<26;++a)g_binds[d][c][a]=legacy_default_binding(d,c,a);
  if(c>=4 && c<=10)g_binds[d][c][15]=d?9:'O';
  if(c==3 || c==2){g_listen=15;accept_binding(d?10:'P');g_release=0;}
  save_context();
 }
 g_initialized=0;recomp_controls_init((HWND)1);
 const unsigned joy[]={6,7,8,11};
 for(unsigned j=0;j<4;++j)for(int d=0;d<2;++d){
  reset_input();play(joy[j]);assert(g_context==(joy[j]==11?2:3));
  state=(XBOX_INPUT_STATE){0};
  if(d)state.Gamepad.bAnalogButtons[2]=255;else test_keys['P']=1;
  assert(mapper(&state,!d));assert(state.Gamepad.bAnalogButtons[7]==255);
  assert((state.Gamepad.bAnalogButtons[2]==255)==(d && g_context==3));
  assert(recomp_controls_prompt_binding(15,d?XBOX_PROMPT_PLAYSTATION:XBOX_PROMPT_KEYBOARD,-1)==(d?10:'P'));
  assert(recomp_controls_prompt_binding(15,d?XBOX_PROMPT_PLAYSTATION:XBOX_PROMPT_KEYBOARD,joy[j]-1)==(d?10:'P'));
  reset_input();state=(XBOX_INPUT_STATE){0};
  if(d)state.Gamepad.bAnalogButtons[1]=255;else test_keys['O']=1;
  mapper(&state,!d);assert(!state.Gamepad.bAnalogButtons[7]);
  reset_input();state=(XBOX_INPUT_STATE){0};
  if(d)state.Gamepad.bAnalogButtons[7]=255;else test_keys['E']=1;
  mapper(&state,!d);assert(!state.Gamepad.bAnalogButtons[7]);
 }
 /* The removed contexts really use the shared movement map as well. */
 for(unsigned j=0;j<4;++j){
  reset_input();play(joy[j]);int owner=g_context;
  controls_assign(g_binds[0][owner],19,'T');controls_assign(g_binds[1][owner],19,23);
  test_keys['T']=1;state=(XBOX_INPUT_STATE){0};mapper(&state,TRUE);assert(state.Gamepad.sThumbLY==32767);
  reset_input();state=(XBOX_INPUT_STATE){0};state.Gamepad.sThumbRY=24000;mapper(&state,FALSE);
  assert(abs(state.Gamepad.sThumbLY-24000)<=1 && abs(state.Gamepad.sThumbRY-24000)<=1);
 }
 reset_input();play(4);g_edit_context=3;g_page=0;g_device=0;
}

int main(int argc,char**argv){
 g_xbox_mem_offset=(ptrdiff_t)camera_memory-0x10000;
 assert(argc==2);snprintf(test_ini,sizeof(test_ini),"%s\\runtime.exe",argv[1]);
 /* This regression suite exercises an existing pre-QWERTY profile.
  * Fresh defaults and default-version migration have their own suite. */
 char initial_ini[MAX_PATH];snprintf(initial_ini,sizeof(initial_ini),"%s\\mercenaries_recomp.ini",argv[1]);
 WritePrivateProfileStringA("Controls","KeyboardDefaultsVersion","1",initial_ini);
 recomp_controls_init((HWND)1);assert(registered_mapper==mapper);
 play(4);
 assert(recomp_controls_prompt_binding(11,XBOX_PROMPT_KEYBOARD,-1)=='V');
 assert(recomp_controls_prompt_binding(11,XBOX_PROMPT_PLAYSTATION,-1)==11);
 controls_assign(g_binds[0][3],11,'E');
 controls_assign(g_binds[1][3],11,10);
 assert(recomp_controls_prompt_binding(11,XBOX_PROMPT_KEYBOARD,-1)=='E');
 assert(recomp_controls_prompt_binding(11,XBOX_PROMPT_PLAYSTATION,-1)==10);
 play(13);assert(recomp_controls_prompt_binding(11,XBOX_PROMPT_KEYBOARD,-1)=='E');
 play(10);assert(recomp_controls_prompt_binding(11,XBOX_PROMPT_KEYBOARD,-1)==0xffffu);
 g_pause_navigation=1;
 assert(recomp_controls_prompt_binding(8,XBOX_PROMPT_KEYBOARD,-1)==VK_RETURN);
 assert(recomp_controls_prompt_binding(9,XBOX_PROMPT_KEYBOARD,-1)==VK_ESCAPE);
 g_pause_navigation=0;
 for(unsigned i=0;i<CONTROL_CHANNELS;++i){g_binds[0][3][i]=keys[i];g_binds[1][3][i]=(unsigned short)i;}
 /* Analog identity, triggers, negative axis endpoints and independent keyboard. */
 for(unsigned context=1;context<=14;++context){
  reset_input();play(context);XBOX_INPUT_STATE state={0};state.Gamepad.sThumbLX=-32768;
  state.Gamepad.sThumbRY=16384;state.Gamepad.bAnalogButtons[6]=127;
  assert(mapper(&state,FALSE));assert(state.Gamepad.sThumbLX==((context==5 || context==10)?0:-32767));
  assert(abs(state.Gamepad.sThumbRY-16384)<=1);assert(state.Gamepad.bAnalogButtons[6]==((context==5 || context==10)?0:127));
 }
 play(4);unsigned short before[CONTROL_CHANNELS];memcpy(before,g_binds[1][3],sizeof(before));
 controls_assign(g_binds[0][3],19,'T');test_keys['T']=1;
 XBOX_INPUT_STATE state={0};assert(mapper(&state,TRUE));assert(state.Gamepad.sThumbLY==32767);
 assert(!memcmp(before,g_binds[1][3],sizeof(before)));test_keys['T']=0;test_keys['W']=1;
 mapper(&state,TRUE);assert(state.Gamepad.sThumbLY==0);reset_input();
 /* Explicitly shared opposing axes cancel; other shared actions both fire. */
 controls_assign(g_binds[0][3],19,'S');assert(g_binds[0][3][18]=='S');
 test_keys['S']=test_keys['T']=1;mapper(&state,TRUE);assert(state.Gamepad.sThumbLY==0);reset_input();
 controls_assign(g_binds[1][3],15,8);memcpy(before,g_binds[0][3],sizeof(before));
 state=(XBOX_INPUT_STATE){0};state.Gamepad.bAnalogButtons[0]=255;mapper(&state,FALSE);
 assert(state.Gamepad.bAnalogButtons[7]==255 && state.Gamepad.bAnalogButtons[0]==255);
 assert(!memcmp(before,g_binds[0][3],sizeof(before)));
 /* Pause navigation inherits On Foot, not the active vehicle context.
  * Menu accept/back remain usable and no physical stick bypasses a rebind. */
 unsigned short saved_keyboard[24],saved_pad[24];
 memcpy(saved_keyboard,g_binds[0][3],sizeof(saved_keyboard));
 memcpy(saved_pad,g_binds[1][3],sizeof(saved_pad));
 for(unsigned i=0;i<24;++i){g_binds[0][3][i]=0xffffu;g_binds[1][3][i]=0xffffu;}
 const unsigned movement[]={19,18,16,17};const unsigned navkeys[]={'T','G','F','H'};
 const unsigned padsources[]={23,22,20,21};
 for(unsigned i=0;i<4;++i){g_binds[0][3][movement[i]]=navkeys[i];g_binds[1][3][movement[i]]=padsources[i];}
 for(unsigned joy=1;joy<=14;++joy){
  play(joy);recomp_controls_state(0x4249D707,0x7084D38D,joy,0);
  assert(g_pause_navigation && !g_gameplay);
  for(unsigned i=0;i<4;++i){
   reset_input();test_keys[navkeys[i]]=1;state=(XBOX_INPUT_STATE){0};mapper(&state,TRUE);
   assert((state.Gamepad.wButtons&15)==(1u<<i));
   float input[24]={0};input[padsources[i]]=1;state=(XBOX_INPUT_STATE){0};pack(&state.Gamepad,input);
   mapper(&state,FALSE);assert((state.Gamepad.wButtons&15)==(1u<<i));
   assert(!state.Gamepad.sThumbLX && !state.Gamepad.sThumbLY && !state.Gamepad.sThumbRX && !state.Gamepad.sThumbRY);
  }
 }
 reset_input();test_keys['T']=test_keys['G']=1;mapper(&state,TRUE);assert(!(state.Gamepad.wButtons&15));
 reset_input();test_keys['W']=1;mapper(&state,TRUE);assert(!(state.Gamepad.wButtons&15));reset_input();
 state=(XBOX_INPUT_STATE){0};state.Gamepad.sThumbLY=32767;mapper(&state,FALSE);assert(!(state.Gamepad.wButtons&15));
 g_binds[0][12][4]='P';test_keys['P']=test_keys[VK_RETURN]=1;mapper(&state,TRUE);
 assert((state.Gamepad.wButtons&0x10) && state.Gamepad.bAnalogButtons[0]==255);reset_input();
 state=(XBOX_INPUT_STATE){0};state.Gamepad.bAnalogButtons[1]=255;mapper(&state,FALSE);assert(state.Gamepad.bAnalogButtons[1]==255);
 /* Same navigation inside rebinding, including an unfocused controller. */
 recomp_controls_open();test_focus=(HWND)2;state=(XBOX_INPUT_STATE){0};state.Gamepad.sThumbRY=32767;
 mapper(&state,FALSE);assert((state.Gamepad.wButtons&15)==1);
 test_keys['T']=1;mapper(&state,TRUE);assert(!state.Gamepad.wButtons);reset_input();
 memcpy(g_binds[0][3],saved_keyboard,sizeof(saved_keyboard));memcpy(g_binds[1][3],saved_pad,sizeof(saved_pad));
 recomp_controls_close();play(12);assert(!g_pause_navigation && g_context==11 && g_mapping_active);play(4);
 /* HQ Briefing and Use Only inherit both devices' On Foot bindings. */
 memcpy(saved_keyboard,g_binds[0][3],sizeof(saved_keyboard));memcpy(saved_pad,g_binds[1][3],sizeof(saved_pad));
 controls_assign(g_binds[0][3],19,'T');controls_assign(g_binds[0][3],11,'U');
 controls_assign(g_binds[1][3],19,23);controls_assign(g_binds[1][3],11,8);
 for(unsigned joy=9;joy<=14;++joy){
  if(joy==10 || joy==11 || joy==12)continue;
  play(joy);assert(g_context==3 && g_mapping_active);
  reset_input();test_keys['T']=test_keys['U']=1;state=(XBOX_INPUT_STATE){0};mapper(&state,TRUE);
  assert(state.Gamepad.sThumbLY==(joy==10?0:32767) && state.Gamepad.bAnalogButtons[3]==255);
  reset_input();state=(XBOX_INPUT_STATE){0};state.Gamepad.sThumbRY=24000;state.Gamepad.bAnalogButtons[0]=255;
  mapper(&state,FALSE);assert(abs(state.Gamepad.sThumbLY-(joy==10?0:24000))<=1 && state.Gamepad.bAnalogButtons[3]==255);
 }
 memcpy(g_binds[0][3],saved_keyboard,sizeof(saved_keyboard));memcpy(g_binds[1][3],saved_pad,sizeof(saved_pad));
 assert(controls_context(0)==-1 && controls_context(15)==-1);play(4);reset_input();
 /* Every channel appears exactly once in the paged action list. */
 unsigned seen=0;for(unsigned i=0;i<24;++i){assert(order[i]<24);seen|=1u<<order[i];}assert(seen==0xffffff);
 recomp_controls_open();assert(recomp_controls_count()==7);
 recomp_controls_input(5,5,1);assert(recomp_controls_count()==9);
 for(int i=0;i<6;++i){g_page=i;for(unsigned row=0;row<9;++row)assert(recomp_controls_label(RECOMP_CONTROLS_ROW+row));}
 g_page=5;recomp_controls_input(6,3,1);assert(g_page==0);recomp_controls_input(6,2,1);assert(g_page==5);
 /* Merged modes are skipped in both directions without renumbering saves. */
 g_edit_context=3;recomp_controls_input(1,3,1);assert(g_edit_context==SCOPE_CONTEXT);
 recomp_controls_input(1,2,1);assert(g_edit_context==3);
 for(int i=0;i<20;++i){recomp_controls_input(1,3,1);assert(contexts[g_edit_context] && !(g_edit_context>=5 && g_edit_context<=10));}
 /* Fixed menu keys survive arbitrary gameplay remapping. */
 test_keys[VK_RETURN]=1;mapper(&state,TRUE);assert(state.Gamepad.bAnalogButtons[0]==255);reset_input();
 /* Opening accept is held: binding capture must not record it. */
 g_device=0;g_edit_context=3;g_page=0;test_keys[VK_RETURN]=1;
 recomp_controls_input(2,5,1);assert(g_listen==19);mapper(&state,TRUE);assert(g_listen==19 && g_capture_held[VK_RETURN]);
 test_keys[VK_RETURN]=0;mapper(&state,TRUE);assert(g_armed);
 test_keys[VK_LBUTTON]=1;mapper(&state,TRUE);assert(g_listen<0 && g_release && g_binds[0][3][19]==VK_LBUTTON);
 assert(!state.Gamepad.wButtons && !state.Gamepad.sThumbLY);mapper(&state,TRUE);assert(g_release);
 test_keys[VK_LBUTTON]=0;mapper(&state,TRUE);assert(!g_release);
 /* F10 cancels without mutation; Backspace unbinds. */
 recomp_controls_input(2,5,1);mapper(&state,TRUE);test_keys[VK_F10]=1;mapper(&state,TRUE);
 assert(g_binds[0][3][19]==VK_LBUTTON);reset_input();
 recomp_controls_input(2,5,1);mapper(&state,TRUE);test_keys[VK_BACK]=1;mapper(&state,TRUE);
 assert(g_binds[0][3][19]==0xffffu);reset_input();
 /* Gamepad: release first, ignore drift, accept a new direction, keyboard unchanged. */
 memcpy(before,g_binds[0][3],sizeof(before));g_device=1;recomp_controls_input(2,5,1);
 state=(XBOX_INPUT_STATE){0};state.Gamepad.bAnalogButtons[0]=255;mapper(&state,FALSE);assert(g_listen==19 && g_capture_held[8]);
 state=(XBOX_INPUT_STATE){0};state.Gamepad.sThumbRX=2000;mapper(&state,FALSE);assert(g_armed);
 state=(XBOX_INPUT_STATE){0};state.Gamepad.sThumbRX=-30000;mapper(&state,FALSE);assert(g_binds[1][3][19]==20 && g_release);
 assert(!memcmp(before,g_binds[0][3],sizeof(before)));reset_input();
 /* Persistence reloads both devices from the isolated INI. */
 g_device=0;save_context();g_device=1;save_context();g_initialized=0;recomp_controls_init((HWND)1);
 assert(g_binds[0][3][19]==0xffffu && g_binds[1][3][19]==20);
 /* Background gamepads retain remaps; keyboard/mouse remain neutral. */
 for(unsigned context=1;context<=14;++context){
  play(context);reset_input();
  XBOX_INPUT_STATE front={0},back={0};front.dwPacketNumber=77;
  front.Gamepad.wButtons=0x10;front.Gamepad.sThumbLX=-24000;
  front.Gamepad.sThumbRY=18000;front.Gamepad.bAnalogButtons[0]=255;
  front.Gamepad.bAnalogButtons[6]=123;back=front;
  mapper(&front,FALSE);test_focus=(HWND)2;mapper(&back,FALSE);
  assert(!memcmp(&front,&back,sizeof(front)));
  test_keys['W']=test_keys[VK_LBUTTON]=1;mapper(&back,TRUE);
  XBOX_INPUT_STATE neutral={0};assert(!memcmp(&back,&neutral,sizeof(back)));reset_input();
 }
 /* Neither binding capture nor fixed menu navigation reads background keys. */
 play(4);recomp_controls_open();test_focus=(HWND)2;
 state=(XBOX_INPUT_STATE){0};state.Gamepad.bAnalogButtons[0]=255;
 assert(mapper(&state,FALSE) && state.Gamepad.bAnalogButtons[0]==255);
 g_listen=19;g_armed=1;g_device=1;g_edit_context=3;
 memcpy(before,g_binds[1][3],sizeof(before));mapper(&state,FALSE);
 assert(!g_armed && g_listen==19 && !memcmp(before,g_binds[1][3],sizeof(before)));reset_input();
 /* Capture policy: play on, menus/PDA/pause/F9/focus/minimize off. */
 play(4);g_mouse=1;g_isolated=0;update_mouse();assert(test_clip && !test_cursor && g_captured);
 test_focus=(HWND)2;recomp_controls_message((HWND)1,WM_KILLFOCUS,0,0);assert(!test_clip && test_cursor && !g_captured);
 test_focus=(HWND)1;update_mouse();assert(g_captured);test_overlay=1;update_mouse();assert(!g_captured);
 test_overlay=0;update_mouse();assert(g_captured);recomp_controls_open();assert(!g_captured);
 recomp_controls_close();play(12);update_mouse();assert(!g_captured);
 play(4);recomp_controls_state(0,0,4,1);update_mouse();assert(!g_captured);
 play(4);test_is_iconic=1;update_mouse();assert(!g_captured);test_is_iconic=0;
 /* Mouse counts are consumed once across retail substeps; no inertia tail. */
 /* Free camera always uses independently rebound On Foot channels, even
  * if enabled from a vehicle. Gameplay actions are consumed after mapping. */
 reset_input();play(1);recomp_freecam_set(1);controls_assign(g_binds[0][3],19,'T');test_keys['T']=1;
 mapper(&state,TRUE);assert(state.Gamepad.sThumbLY>30000);
 recomp_controls_freecam_input(&state);assert(state.Gamepad.sThumbLY==0);
 /* R/rebound Reload no longer ascends. Space owns ascent without leaking
  * another action or moving the mercenary, and gamepad A remains mapped. */
 reset_input();g_binds[0][3][8]='R';test_keys['R']=1;
 mapper(&state,TRUE);assert(state.Gamepad.bAnalogButtons[0]==0);
 reset_input();test_keys[VK_SPACE]=1;mapper(&state,TRUE);
 assert(state.Gamepad.bAnalogButtons[0]==255);
 recomp_controls_freecam_input(&state);assert(state.Gamepad.bAnalogButtons[0]==0);
 g_xbox_mem_offset=(ptrdiff_t)camera_memory-0x10000;camera_matrix(0x11000);
 recomp_freecam_camera(0x10000,0x11000,.1f);assert(MEMF(0x11034)>20.f);g_xbox_mem_offset=(ptrdiff_t)camera_memory-0x10000;
 reset_input();g_binds[1][3][8]=8;state=(XBOX_INPUT_STATE){0};state.Gamepad.bAnalogButtons[0]=255;
 mapper(&state,FALSE);assert(state.Gamepad.bAnalogButtons[0]==255);
 reset_input();recomp_freecam_set(0);play(4);g_binds[0][3][19]=legacy_default_binding(0,3,19);
 reset_input();test_keys['R']=1;mapper(&state,TRUE);assert(state.Gamepad.bAnalogButtons[0]==255);
 reset_input();g_binds[0][3][8]=legacy_default_binding(0,3,8);
 /* F9 permits only host camera movement; the retail packet remains neutral.
  * A panel with focus accepts rebound movement, but text entry/Alt-Tab stop it. */
 reset_input();play(4);camera_matrix(0x11000);recomp_freecam_set(1);
 test_overlay=test_overlay_camera=1;test_focus=(HWND)2;
 g_binds[0][3][19]='T';test_keys['T']=1;
 recomp_controls_freecam_input(&state);assert(state.Gamepad.sThumbLY==0);
 recomp_freecam_camera(0x10000,0x11000,0);float camera_z=MEMF(0x11038);assert(camera_z<30);
 test_text_entry=1;recomp_controls_freecam_input(&state);recomp_freecam_camera(0x10000,0x11000,0);assert(MEMF(0x11038)==camera_z);
 test_text_entry=0;test_focus=NULL;recomp_controls_freecam_input(&state);recomp_freecam_camera(0x10000,0x11000,0);assert(MEMF(0x11038)==camera_z);
 test_focus=(HWND)2;test_keys['T']=0;test_overlay_pad=1;
 for(unsigned i=0;i<CONTROL_ACTIONS;i++)g_binds[1][3][i]=legacy_default_binding(1,3,i);
 recomp_controls_freecam_input(&state);recomp_freecam_camera(0x10000,0x11000,0);assert(MEMF(0x11038)<camera_z && state.Gamepad.sThumbLY==0);
 camera_z=MEMF(0x11038);g_modal=1;recomp_controls_freecam_input(&state);recomp_freecam_camera(0x10000,0x11000,0);assert(MEMF(0x11038)==camera_z);g_modal=0;
 test_overlay_pad=0;test_keys['Q']=1;assert(recomp_controls_freecam_roll()==1);test_text_entry=1;assert(recomp_controls_freecam_roll()==0);test_text_entry=0;
 update_mouse();assert(!g_captured);
 recomp_freecam_set(0);test_keys['T']=1;mapper(&state,TRUE);assert(state.Gamepad.sThumbLY==0);
 recomp_controls_freecam_input(&state);assert(state.Gamepad.sThumbLY==0);
 test_overlay_camera=0;play(4);reset_input();g_binds[0][3][19]=legacy_default_binding(0,3,19);
 /* F10 hides only HUD paint, repeats do not toggle, key-up is consumed.
  * Rebind cancellation and background input never change visibility. */
 g_isolated=0;g_hud_hidden=0;play(4);
 assert(!recomp_controls_hide_hud_brush(0x361FC0,0x364028));
 assert(recomp_controls_message((HWND)1,WM_KEYDOWN,VK_F10,0));
 assert(g_hud_hidden && recomp_controls_hide_hud_brush(0x361FC0,0x364028));
 assert(!recomp_controls_hide_hud_brush(0x35CFB8,0x363FD0));
 assert(!recomp_controls_hide_hud_brush(0x342F80,0x342F30));
 recomp_controls_message((HWND)1,WM_KEYDOWN,VK_F10,1L<<30);assert(g_hud_hidden);
 assert(recomp_controls_message((HWND)1,WM_SYSKEYUP,VK_F10,0));assert(g_hud_hidden);
 g_menu=1;assert(!recomp_controls_hide_hud_brush(0x361FC0,0x364028));g_menu=0;
 g_modal=1;assert(!recomp_controls_hide_hud_brush(0x361FC0,0x364028));g_modal=0;
 test_focus=(HWND)2;recomp_controls_message((HWND)1,WM_KEYDOWN,VK_F10,0);assert(g_hud_hidden);
 test_focus=(HWND)1;g_listen=19;recomp_controls_message((HWND)1,WM_KEYDOWN,VK_F10,0);
 assert(g_listen==-1 && g_release && g_hud_hidden);reset_input();
 recomp_controls_message((HWND)1,WM_SYSKEYDOWN,VK_F10,0);assert(!g_hud_hidden);

 /* Crouch hold cancellation must see mouse movement without consuming it. */
 update_mouse();g_dx=g_dy=0;assert(!recomp_controls_mouse_look_pending());
 g_dx=3;assert(recomp_controls_mouse_look_pending() && g_dx==3);
 g_dx=0;g_dy=-4;assert(recomp_controls_mouse_look_pending() && g_dy==-4);
 g_menu=1;assert(!recomp_controls_mouse_look_pending());g_menu=0;
 g_mouse=0;assert(!recomp_controls_mouse_look_pending());g_mouse=1;
 g_gameplay=0;assert(!recomp_controls_mouse_look_pending());g_gameplay=1;
 g_captured=0;assert(!recomp_controls_mouse_look_pending());g_captured=1;
 g_test_mouse_path="synthetic";g_test_dx=0;g_test_dy=7;
 assert(recomp_controls_mouse_look_pending() && g_test_dy==7);
 g_test_dx=g_test_dy=0;assert(!recomp_controls_mouse_look_pending());g_test_mouse_path=NULL;
 g_dx=g_dy=0;
 update_mouse();g_sensitivity=5;g_dx=100;float x=recomp_controls_mouse_delta(0,.01f);
 assert(fabsf(x-.21f)<.00001f);assert(g_dx==0);assert(fabsf(recomp_controls_mouse_delta(0,.01f)-.01f)<.000001f);
 g_dy=50;assert(fabsf(recomp_controls_mouse_delta(1,0)+.1f)<.00001f);
 g_invert=1;g_dy=50;assert(fabsf(recomp_controls_mouse_delta(1,0)-.1f)<.00001f);g_invert=0;
 float sum=0;for(int i=0;i<6;++i){g_dx=10;sum+=recomp_controls_mouse_delta(0,0);}g_dx=60;
 assert(fabsf(sum-recomp_controls_mouse_delta(0,0))<.00001f);
 /* Vehicle rate integrates to the same movement at 30 / 60 updates. */
 play(1);update_mouse();g_dx=4;float a=recomp_controls_mouse_axis(0xEAA0CE27,0,1.f/30);
 g_dx=2;float b=recomp_controls_mouse_axis(0xEAA0CE27,0,1.f/60);assert(fabsf(a-b)<.00001f);
 /* Sniper shares bindings but still uses the scope's mechanical mouse path. */
 play(10);update_mouse();assert(g_context==SCOPE_CONTEXT && g_joystick_context==9);
 g_dx=2;assert(recomp_controls_mouse_axis(0xEAA0CE27,0,1.f/60)>0);assert(!g_dx);
 play(4);g_dx=2;assert(recomp_controls_mouse_axis(0xEAA0CE27,0,1.f/60)==0);assert(g_dx==2);
 g_dx=10;recomp_controls_open();assert(g_dx==0 && !g_captured);

 /* Rebindable menu context: AZERTY navigation, explicit confirm/back and no
  * legacy Z/X or grenade channels in shell, pause, help and support. */
 reset_input();recomp_controls_close();play(4);
 for(int d=0;d<2;++d)for(unsigned a=0;a<26;++a){
  g_binds[d][3][a]=legacy_default_binding(d,3,a);g_binds[d][12][a]=legacy_default_binding(d,12,a);}
 test_azerty=1;assert(legacy_default_binding(0,3,19)=='Z' && legacy_default_binding(0,3,16)=='Q');
 assert(legacy_default_binding(0,3,14)=='A' && legacy_default_binding(0,3,8)=='W');test_azerty=0;
 controls_assign(g_binds[0][3],19,'Z');controls_assign(g_binds[0][3],16,'Q');
 controls_assign(g_binds[0][3],14,'A'); /* Explicit grenade rebind; no automatic swaps. */
 const unsigned substates[]={0,4,5,1,2};
 for(unsigned m=0;m<5;++m){
  reset_input();play(4);if(m==0)recomp_controls_state(0x59C00449,0xC2CBD863,4,0);
  else {recomp_controls_state(0x4249D707,0xDDFB69D8,4,1);recomp_controls_substate(substates[m]);}
  test_keys['Z']=test_keys['Q']=1;mapper(&state,TRUE);
  assert((state.Gamepad.wButtons&15)==5 && !state.Gamepad.bAnalogButtons[0] && !state.Gamepad.bAnalogButtons[6]);
  memset(test_keys,0,sizeof(test_keys));test_keys[VK_RETURN]=1;mapper(&state,TRUE);
  assert(state.Gamepad.bAnalogButtons[0]==255 && !(state.Gamepad.wButtons&0x10));
  assert(recomp_controls_prompt_binding(8,XBOX_PROMPT_KEYBOARD,-1)==VK_RETURN);
  if(g_support)assert(recomp_controls_prompt_binding(15,XBOX_PROMPT_KEYBOARD,-1)==resolved_binding(0,MENU_CONTEXT,15));
  test_keys[VK_RETURN]=0;test_keys[VK_ESCAPE]=1;mapper(&state,TRUE);
  assert(state.Gamepad.bAnalogButtons[g_modal_back_y?3:1]==255);
 }
 assert(legacy_default_binding(1,MENU_CONTEXT,11)==11);
 assert(legacy_default_binding(0,MENU_CONTEXT,11)==legacy_default_binding(0,3,11));
 assert(!strcmp(action_name(MENU_CONTEXT,11),"DECLINE SIDE MISSION"));
 /* Side-mission Yes/No: menu A confirms, Menu Decline declines. Both input
  * and glyphs follow custom bindings; ordinary menu Back never declines. */
 for(int device=0;device<2;++device){
  reset_input();play(4);
  recomp_controls_state(0x4249D707,0xDDFB69D8,4,1);recomp_controls_substate(5);
  MEM32(0x35F798+0x30)=1;MEM32(0x35F798+0x4B4)=1;
  g_binds[device][MENU_CONTEXT][8]=device?10:'K';
  g_binds[device][MENU_CONTEXT][9]=device?9:VK_ESCAPE;
  g_binds[device][MENU_CONTEXT][11]=device?12:'F';
  const int family=device?XBOX_PROMPT_PLAYSTATION:XBOX_PROMPT_KEYBOARD;
  assert(mission_yes_no_active());
  MEM32(0x35F798+0x30)=3;assert(!mission_yes_no_active());MEM32(0x35F798+0x30)=1;
  assert(recomp_controls_prompt_binding(8,family,-1)==(device?10:'K'));
  assert(recomp_controls_prompt_binding(11,family,-1)==(device?12:'F'));
  memset(&state,0,sizeof(state));if(device)state.Gamepad.bAnalogButtons[2]=255;else test_keys['K']=1;
  mapper(&state,!device);assert(state.Gamepad.bAnalogButtons[0]==255 && !state.Gamepad.bAnalogButtons[3]);
  reset_input();memset(&state,0,sizeof(state));if(device)state.Gamepad.bAnalogButtons[4]=255;else test_keys['F']=1;
  mapper(&state,!device);assert(state.Gamepad.bAnalogButtons[3]==255 && !state.Gamepad.bAnalogButtons[0] && !state.Gamepad.bAnalogButtons[4]);
  reset_input();memset(&state,0,sizeof(state));if(device)state.Gamepad.bAnalogButtons[1]=255;else test_keys[VK_ESCAPE]=1;
  mapper(&state,!device);assert(!state.Gamepad.bAnalogButtons[3] && !state.Gamepad.bAnalogButtons[1]);
  /* Both Back and Decline may share B/Escape without swaps. Only the
   * dialog-specific destination is emitted while this Yes/No box is open. */
  g_device=device;g_edit_context=MENU_CONTEXT;g_listen=11;
  accept_binding(device?9:VK_ESCAPE);g_release=0;
  assert(g_binds[device][MENU_CONTEXT][9]==(device?9:VK_ESCAPE));
  assert(g_binds[device][MENU_CONTEXT][11]==(device?9:VK_ESCAPE));
  reset_input();memset(&state,0,sizeof(state));if(device)state.Gamepad.bAnalogButtons[1]=255;else test_keys[VK_ESCAPE]=1;
  mapper(&state,!device);assert(state.Gamepad.bAnalogButtons[3]==255 && !state.Gamepad.bAnalogButtons[1]);
  assert(recomp_controls_prompt_binding(11,family,-1)==(device?9:VK_ESCAPE));
  /* Explicit unbinding stays unbound; no fallback to default Y/Back. */
  g_binds[device][MENU_CONTEXT][11]=0xffff;
  assert(recomp_controls_prompt_binding(11,family,-1)==0xffff);
  memset(&state,0,sizeof(state));if(device)state.Gamepad.bAnalogButtons[3]=255;else test_keys['V']=1;
  mapper(&state,!device);assert(!state.Gamepad.bAnalogButtons[3]);
  /* Continue tutorials, support and frontend menus retain their old map,
   * even with the stale Yes/No type present in the retail object. */
  MEM32(0x35F798+0x4B4)=0;
  assert(!mission_yes_no_active());assert(recomp_controls_prompt_binding(11,family,-1)==resolved_binding(device,MENU_CONTEXT,9));
  MEM32(0x35F798+0x4B4)=1;
  recomp_controls_substate(1);assert(!mission_yes_no_active());
  assert(recomp_controls_prompt_binding(11,family,-1)==resolved_binding(device,MENU_CONTEXT,9));
  reset_input();memset(&state,0,sizeof(state));if(device)state.Gamepad.bAnalogButtons[1]=255;else test_keys[VK_ESCAPE]=1;
  mapper(&state,!device);assert(state.Gamepad.bAnalogButtons[3]==255);
  recomp_controls_state(0x59C00449,0xC2CBD863,4,0);assert(!mission_yes_no_active());
  reset_input();memset(&state,0,sizeof(state));if(device)state.Gamepad.bAnalogButtons[1]=255;else test_keys[VK_ESCAPE]=1;
  mapper(&state,!device);assert(state.Gamepad.bAnalogButtons[1]==255 && !state.Gamepad.bAnalogButtons[3]);
  g_binds[device][MENU_CONTEXT][8]=legacy_default_binding(device,MENU_CONTEXT,8);
  g_binds[device][MENU_CONTEXT][11]=legacy_default_binding(device,MENU_CONTEXT,11);
 }
 MEM32(0x35F798+0x4B4)=0;
 for(int d=0;d<2;++d){
  g_device=d;g_edit_context=3;save_context();
  g_edit_context=MENU_CONTEXT;
  g_listen=9;accept_binding(d?9:VK_ESCAPE);
  g_listen=11;accept_binding(d?9:VK_ESCAPE);
 }
 g_initialized=0;recomp_controls_init((HWND)1);
 for(int d=0;d<2;++d){
  assert(g_binds[d][MENU_CONTEXT][9]==(d?9:VK_ESCAPE));
  assert(g_binds[d][MENU_CONTEXT][11]==(d?9:VK_ESCAPE));
  g_device=d;g_edit_context=MENU_CONTEXT;g_listen=11;accept_binding(0xffff);
  assert(g_binds[d][MENU_CONTEXT][9]==(d?9:VK_ESCAPE));
 }
 /* Support-add owns menu input even when a takedown's weapon re-equip
  * restores the joystick substate to Default (or its table is Disabled).
  * Test the actual mapper, including mouse, keyboard, rebound gamepad,
  * and the held-fire barrier on returning to normal play. */
 for(unsigned joy=1;joy<=15;++joy)for(unsigned sub=0;sub<=2;sub+=2)
 for(int enabled=0;enabled<=1;++enabled)for(int d=0;d<2;++d){
  unsigned short old_confirm=g_binds[d][MENU_CONTEXT][8];
  unsigned short old_select=g_binds[d][MENU_CONTEXT][15];
  unsigned short old_fire=g_binds[d][3][15];
  g_binds[d][MENU_CONTEXT][8]=d?10:VK_LBUTTON;
  g_binds[d][MENU_CONTEXT][15]=d?15:'E';
  g_binds[d][3][15]=d?10:VK_LBUTTON;
  for(unsigned select=0;select<2;++select){
   reset_input();g_menu=0;test_focus=(HWND)1;test_overlay=0;
   recomp_controls_state(0x4249D707,0xC739FD0F,joy,enabled);
   recomp_controls_substate(sub);
   assert(menu_active() && g_support);
   memset(&state,0,sizeof(state));
   if(d)state.Gamepad.bAnalogButtons[select?7:2]=255;
   else test_keys[select?'E':VK_LBUTTON]=1;
   assert(mapper(&state,!d));
   assert(state.Gamepad.bAnalogButtons[0]==255 && !state.Gamepad.bAnalogButtons[7]);
   assert(recomp_controls_prompt_binding(8,d?XBOX_PROMPT_XBOX:XBOX_PROMPT_KEYBOARD,-1)==(d?10:VK_LBUTTON));
   play(4);recomp_controls_substate(0);assert(!menu_active());
   memset(&state,0,sizeof(state));
   if(d)state.Gamepad.bAnalogButtons[select?7:2]=255;
   mapper(&state,!d);assert(!state.Gamepad.bAnalogButtons[7]);
  }
  g_binds[d][MENU_CONTEXT][8]=old_confirm;
  g_binds[d][MENU_CONTEXT][15]=old_select;
  g_binds[d][3][15]=old_fire;
 }
 /* Support has two accept bindings but emits only one retail accept event.
  * Selection never activates equipped Fire on returning to gameplay. */
 for(int d=0;d<2;++d){
  reset_input();play(4);recomp_controls_substate(1);
  unsigned short saved_confirm=g_binds[d][MENU_CONTEXT][8];
  unsigned short saved_select=g_binds[d][MENU_CONTEXT][15];
  unsigned short saved_fire=g_binds[d][3][15];
  g_binds[d][MENU_CONTEXT][8]=d?10:'K';
  g_binds[d][MENU_CONTEXT][15]=d?15:'P';
  g_binds[d][3][15]=d?15:'P';
  for(unsigned sub=1;sub<=2;++sub)for(unsigned which=0;which<3;++which){
   reset_input();play(4);recomp_controls_substate(sub);memset(&state,0,sizeof(state));
   if(d){if(which!=1)state.Gamepad.bAnalogButtons[2]=255;if(which!=0)state.Gamepad.bAnalogButtons[7]=255;}
   else{test_keys['K']=which!=1;test_keys['P']=which!=0;}
   mapper(&state,!d);assert(state.Gamepad.bAnalogButtons[0]==255 && !state.Gamepad.bAnalogButtons[7]);
   assert(recomp_controls_prompt_binding(8,d?XBOX_PROMPT_PLAYSTATION:XBOX_PROMPT_KEYBOARD,-1)==(d?10:'K'));
   assert(recomp_controls_prompt_support_binding(d?XBOX_PROMPT_PLAYSTATION:XBOX_PROMPT_KEYBOARD)==(d?15:'P'));
  }
  /* Held select is consumed on menu exit; releasing restores Fire. */
  play(4);recomp_controls_substate(0);memset(&state,0,sizeof(state));
  if(d)state.Gamepad.bAnalogButtons[7]=255;
  mapper(&state,!d);assert(!state.Gamepad.bAnalogButtons[7]);
  test_keys['P']=test_keys['K']=0;memset(&state,0,sizeof(state));mapper(&state,!d);
  if(d)state.Gamepad.bAnalogButtons[7]=255;else test_keys['P']=1;
  mapper(&state,!d);assert(state.Gamepad.bAnalogButtons[7]==255);
  /* Same-source Confirm/Select remains a single output. */
  g_binds[d][MENU_CONTEXT][8]=d?15:'P';reset_input();play(4);recomp_controls_substate(1);
  memset(&state,0,sizeof(state));if(d)state.Gamepad.bAnalogButtons[7]=255;else test_keys['P']=1;
  mapper(&state,!d);assert(state.Gamepad.bAnalogButtons[0]==255 && !state.Gamepad.bAnalogButtons[7]);
  g_binds[d][MENU_CONTEXT][8]=d?10:'K';
  /* Unbinding Select retains the independent Confirm route. */
  g_binds[d][MENU_CONTEXT][15]=0xffff;reset_input();play(4);recomp_controls_substate(1);
  memset(&state,0,sizeof(state));if(d)state.Gamepad.bAnalogButtons[2]=255;else test_keys['K']=1;
  mapper(&state,!d);assert(state.Gamepad.bAnalogButtons[0]==255 && !state.Gamepad.bAnalogButtons[7]);
  assert(recomp_controls_prompt_support_binding(d?XBOX_PROMPT_PLAYSTATION:XBOX_PROMPT_KEYBOARD)==0xffff);
  g_binds[d][MENU_CONTEXT][15]=d?15:'P';
  /* The dedicated binding does nothing in the ordinary frontend. */
  reset_input();recomp_controls_state(0x59C00449,0xC2CBD863,4,0);memset(&state,0,sizeof(state));
  if(d)state.Gamepad.bAnalogButtons[7]=255;else test_keys['P']=1;
  mapper(&state,!d);assert(!state.Gamepad.bAnalogButtons[0] && !state.Gamepad.bAnalogButtons[7]);
  g_binds[d][MENU_CONTEXT][8]=saved_confirm;g_binds[d][MENU_CONTEXT][15]=saved_select;g_binds[d][3][15]=saved_fire;
 }
 /* The verification result changes the main substate before its joystick
  * substate, then resets the joystick substate to Default when the card opens.
  * Exercise the complete sequence, including that no-modal fallback gap. */
 const unsigned results[]={0x65872ADE,0xA64B64FC,0xA11AED03,0xF5A9EFA7};
 for(unsigned r=0;r<4;++r){
  for(unsigned modal=0;modal<=4;modal+=4){
   reset_input();recomp_controls_state(0x4249D707,results[r],4,1);recomp_controls_substate(modal);
   test_keys['Q']=1;
   assert(mapper(&state,TRUE));assert(state.Gamepad.sThumbLX==-32767 && !state.Gamepad.wButtons);
   assert(!state.Gamepad.bAnalogButtons[6]); /* Q moves, not default grenade */
   test_keys['A']=1;mapper(&state,TRUE);assert(state.Gamepad.bAnalogButtons[6]==255); /* custom grenade still works */
   state=(XBOX_INPUT_STATE){0};state.Gamepad.sThumbLY=22000;state.Gamepad.bAnalogButtons[6]=255;
   assert(mapper(&state,FALSE));assert(state.Gamepad.sThumbLY==22000 && state.Gamepad.bAnalogButtons[6]==255);
  }
  reset_input();recomp_controls_state(0x4249D707,results[r],15,0);recomp_controls_substate(0);
  test_keys['Q']=1;assert(mapper(&state,TRUE));assert(state.Gamepad.wButtons==4 && !state.Gamepad.bAnalogButtons[6]);
  test_keys['Q']=0;test_keys[VK_RETURN]=1;mapper(&state,TRUE);assert(state.Gamepad.bAnalogButtons[0]==255);
  assert(recomp_controls_prompt_binding(8,XBOX_PROMPT_KEYBOARD,-1)==VK_RETURN);
  reset_input();play(4);test_keys['Q']=1;mapper(&state,TRUE);assert(state.Gamepad.sThumbLX==-32767);
 }
 reset_input();recomp_controls_open();g_menu=2;g_edit_context=12;g_page=0;g_device=0;
 assert(recomp_controls_count()==9 && action_at(0)==0 && page_count()==3);
 g_page=1;assert(recomp_controls_count()==9 && action_at(0)==8 && action_at(1)==11 && action_at(2)==15 && action_at(3)==9);
 g_listen=8;accept_binding('F');g_release=0;test_keys['F']=1;mapper(&state,TRUE);
 assert(state.Gamepad.bAnalogButtons[0]==255 && recomp_controls_prompt_binding(8,XBOX_PROMPT_KEYBOARD,-1)=='F');
 reset_input();test_keys[VK_RETURN]=1;mapper(&state,TRUE);assert(!state.Gamepad.bAnalogButtons[0]);
 reset_input();g_listen=0;accept_binding('T');g_release=0;test_keys['Z']=1;mapper(&state,TRUE);assert(!(state.Gamepad.wButtons&1));
 test_keys['Z']=0;test_keys['T']=1;mapper(&state,TRUE);assert(state.Gamepad.wButtons&1);
 assert(g_binds[0][3][19]=='Z'); /* menu edit cannot change walking */
 reset_input();g_device=1;g_listen=8;accept_binding(10);g_release=0;
 state=(XBOX_INPUT_STATE){0};state.Gamepad.bAnalogButtons[2]=255;mapper(&state,FALSE);
 assert(state.Gamepad.bAnalogButtons[0]==255 && state.Gamepad.bAnalogButtons[2]==255);
 assert(recomp_controls_prompt_binding(8,XBOX_PROMPT_PLAYSTATION,-1)==10);
 /* Movies and title/disconnect use custom Menu Confirm, never default Z. */
 for(unsigned main=0;main<2;++main){
  reset_input();recomp_controls_close();recomp_controls_state(main?0x59C00449:0xAEF39CCF,0,15,0);
  test_keys['F']=1;mapper(&state,TRUE);assert(state.Gamepad.bAnalogButtons[0]==255);
  test_keys['F']=0;test_keys['Z']=1;mapper(&state,TRUE);assert(!state.Gamepad.bAnalogButtons[0]);
  recomp_controls_waiting_controller(1);test_keys['F']=1;mapper(&state,TRUE);
  assert(state.Gamepad.wButtons==0x10 && !state.Gamepad.bAnalogButtons[0]);
  assert(recomp_controls_prompt_binding(4,XBOX_PROMPT_KEYBOARD,-1)=='F');
  recomp_controls_waiting_controller(0);
 }
 /* Acquisition must preserve Fire across GPS Default/ManualTargeting,
  * while menus and different vehicle contexts still reject stale packets. */
 for(unsigned joy=6;joy<=7;++joy)for(unsigned sub=0;sub<=3;sub+=3){
  reset_input();play(joy);recomp_controls_substate(sub);
  assert(recomp_controls_event_current(0x4249D707,0xC2CBD863,joy,3-sub));
  assert(!recomp_controls_event_current(0x4249D707,0xC2CBD863,joy,1));
  assert(!recomp_controls_event_current(0x4249D707,0x7084D38D,joy,sub));
 }
 /* Mouse wheel capture and one press/release per detent at retail polls. */
 reset_input();g_device=0;g_edit_context=SCOPE_CONTEXT;g_listen=24;g_armed=1;
 recomp_controls_message((HWND)1,WM_MOUSEWHEEL,MAKEWPARAM(0,WHEEL_DELTA),0);
 assert(g_listen==-1 && g_binds[0][SCOPE_CONTEXT][24]==WHEEL_UP);reset_input();
 recomp_controls_close();play(10);g_wheel_pending=0;
 recomp_controls_message((HWND)1,WM_MOUSEWHEEL,MAKEWPARAM(0,2*WHEEL_DELTA),0);
 play(10);mapper(&state,TRUE);assert(state.Gamepad.sThumbLY==32767 && !state.Gamepad.wButtons);
 play(10);mapper(&state,TRUE);assert(state.Gamepad.sThumbLY==0);
 play(10);mapper(&state,TRUE);assert(state.Gamepad.sThumbLY==32767);
 play(10);mapper(&state,TRUE);assert(state.Gamepad.sThumbLY==0);
 play(4);g_wheel_pending=1;play(4);mapper(&state,TRUE);assert(!state.Gamepad.sThumbLY); /* wheel does not walk */
 /* Shared wheel binds persist, zoom only in either scope, switch outside.
  * Keyboard movement no longer secretly changes zoom; buttons stay 1x. */
 reset_input();g_edit_context=3;g_device=0;g_listen=12;accept_binding(WHEEL_UP);g_release=0;
 assert(g_binds[0][SCOPE_CONTEXT][24]==WHEEL_UP && g_binds[0][3][12]==WHEEL_UP);
 for(unsigned joy=5;joy<=10;joy+=5){
  reset_input();g_wheel_release=0;g_wheel_pending=1;play(joy);mapper(&state,TRUE);
  assert(state.Gamepad.sThumbLY==32767 && !state.Gamepad.bAnalogButtons[4]);
  assert(recomp_controls_prompt_binding(19,XBOX_PROMPT_KEYBOARD,-1)==WHEEL_UP);
  assert(recomp_controls_prompt_binding(18,XBOX_PROMPT_KEYBOARD,-1)==WHEEL_DOWN);
  g_mouse=0;assert(recomp_controls_mouse_axis(0xD4F1922B,-1,.016f)==-4);
  assert(recomp_controls_mouse_axis(0xD4F1922B,-1,.016f)==-1);g_mouse=1;
  play(joy);test_keys['Z']=1;mapper(&state,TRUE);assert(!state.Gamepad.sThumbLY);
  g_binds[0][SCOPE_CONTEXT][24]='T';test_keys['T']=1;mapper(&state,TRUE);assert(state.Gamepad.sThumbLY==32767);
  assert(recomp_controls_mouse_axis(0xD4F1922B,-1,.016f)==-1);g_binds[0][SCOPE_CONTEXT][24]=WHEEL_UP;
 }
 reset_input();g_wheel_release=0;g_wheel_pending=1;play(4);mapper(&state,TRUE);
 assert(!state.Gamepad.sThumbLY && state.Gamepad.bAnalogButtons[4]==255);

 g_wheel_pending=2;recomp_controls_message((HWND)1,WM_KILLFOCUS,0,0);assert(!g_wheel_pending && !g_wheel_active);
 recomp_controls_open();assert(recomp_controls_aim_assist());recomp_controls_input(3,5,1);assert(!recomp_controls_aim_assist());
 g_initialized=0;recomp_controls_init((HWND)1);assert(!recomp_controls_aim_assist());
 /* Context transition audit: menu Back held across resume cannot turn into
  * a horn/attack. Suppression follows the physical binding, on either device,
  * until release, without blocking independently held movement. */
 for(int d=0;d<2;++d)for(int ctx=0;ctx<CONTROL_CONTEXTS;++ctx)for(unsigned a=0;a<26;++a)
  g_binds[d][ctx][a]=legacy_default_binding(d,ctx,a);
 for(unsigned joy=1;joy<=14;++joy){
  reset_input();play(joy);controls_assign(g_binds[0][g_context],9,VK_ESCAPE);
  recomp_controls_state(0x4249D707,0x7084D38D,joy,0);recomp_controls_substate(0);
  test_keys[VK_ESCAPE]=1;mapper(&state,TRUE);assert(state.Gamepad.bAnalogButtons[1]==255);
  assert(!recomp_controls_event_current(0x4249D707,0xC2CBD863,joy,0));
  play(joy);recomp_controls_substate(0);mapper(&state,TRUE);
  assert(state.Gamepad.bAnalogButtons[1]==0);
  if(joy==7)assert(!(state.Gamepad.wButtons&0x80));
  test_keys[VK_ESCAPE]=0;mapper(&state,TRUE);test_keys[VK_ESCAPE]=1;mapper(&state,TRUE);
  assert(state.Gamepad.bAnalogButtons[1]==(g_context==SCOPE_CONTEXT || joy==7?0:255));
  reset_input();recomp_controls_state(0x4249D707,0x7084D38D,joy,0);recomp_controls_substate(0);
  state=(XBOX_INPUT_STATE){0};state.Gamepad.bAnalogButtons[1]=255;mapper(&state,FALSE);
  play(joy);recomp_controls_substate(0);state=(XBOX_INPUT_STATE){0};state.Gamepad.bAnalogButtons[1]=255;
  state.Gamepad.sThumbLY=32767;mapper(&state,FALSE);
  assert(state.Gamepad.bAnalogButtons[1]==0 && state.Gamepad.sThumbLY==(joy==2?0:32767));
  state=(XBOX_INPUT_STATE){0};mapper(&state,FALSE);state.Gamepad.bAnalogButtons[1]=255;mapper(&state,FALSE);
  assert(state.Gamepad.bAnalogButtons[1]==(g_context==SCOPE_CONTEXT || joy==7?0:255));
 }
 /* Holding a remapped movement input through support/pause/help/PDA must
  * resume movement immediately. The same consumed source must not attack. */
 for(unsigned modal=0;modal<=5;++modal){
  if(modal==3)continue;
  for(int keyboard=0;keyboard<=1;++keyboard){
   reset_input();play(4);recomp_controls_substate(0);
   unsigned b=keyboard?'T':8; /* keyboard key or controller A, not a stick */
   g_binds[keyboard?0:1][3][19]=b;
   g_binds[keyboard?0:1][MENU_CONTEXT][0]=b;
   if(modal==0)recomp_controls_state(0x4249D707,0x7084D38D,4,0);
   else recomp_controls_substate(modal);
   state=(XBOX_INPUT_STATE){0};if(keyboard)test_keys[b]=1;else state.Gamepad.bAnalogButtons[0]=255;
   mapper(&state,keyboard);assert(g_menu_held[keyboard?0:1][b]);
   play(4);recomp_controls_substate(0);
   state=(XBOX_INPUT_STATE){0};if(!keyboard)state.Gamepad.bAnalogButtons[0]=255;
   mapper(&state,keyboard);assert(state.Gamepad.sThumbLY==32767);
   g_binds[keyboard?0:1][3][14]=b;
   state=(XBOX_INPUT_STATE){0};if(!keyboard)state.Gamepad.bAnalogButtons[0]=255;
   mapper(&state,keyboard);assert(state.Gamepad.sThumbLY==32767 && !state.Gamepad.bAnalogButtons[6]);
  }
 }
 for(int d=0;d<2;++d)for(int ctx=0;ctx<CONTROL_CONTEXTS;++ctx)for(unsigned a=0;a<26;++a)
  g_binds[d][ctx][a]=legacy_default_binding(d,ctx,a);
 /* Every supported continuous destination, including sticks rebound to
  * buttons, survives a menu-consumed source. Discrete destinations do not. */
 for(int context=0;context<CONTROL_CONTEXTS;++context)for(unsigned action=0;action<26;++action){
  int continuous=(context<=4 && action>=16 && action<24) ||
     ((context==0 || context==1) && (action==8 || action==10)) ||
     (context==SCOPE_CONTEXT && action>=24) || (context==11 && action>=16 && action<24);
  for(int keyboard=0;keyboard<=1;++keyboard)for(unsigned b=0;b<24;++b){
   g_menu_held[keyboard?0:1][b]=1;
   assert(gameplay_source_allowed(keyboard,context,action,b)==continuous);
   g_menu_held[keyboard?0:1][b]=0;
  }
 }
 /* F9's closing-key latch must also discriminate movement from actions. */
 reset_input();play(4);recomp_controls_substate(0);
 test_overlay_consumed=VK_RETURN;test_keys[VK_RETURN]=1;
 g_binds[0][3][19]=VK_RETURN;g_binds[0][3][14]=VK_RETURN;
 mapper(&state,TRUE);assert(state.Gamepad.sThumbLY==32767 && !state.Gamepad.bAnalogButtons[6]);
 test_overlay=1;mapper(&state,TRUE);assert(!state.Gamepad.sThumbLY);
 test_overlay=0;test_overlay_consumed=0;
 for(int d=0;d<2;++d)for(int ctx=0;ctx<CONTROL_CONTEXTS;++ctx)for(unsigned a=0;a<26;++a)
  g_binds[d][ctx][a]=legacy_default_binding(d,ctx,a);
 /* Save-browser secondary choice is bound and emitted on both devices,
  * including rebindings. It remains independent of Confirm and Back. */
 for(int keyboard=0;keyboard<=1;++keyboard){
  reset_input();recomp_controls_state(0x59C00449,0,15,0);recomp_controls_substate(0);
  unsigned device=keyboard?0:1,b=keyboard?VK_DELETE:10;
  assert(recomp_controls_prompt_binding(10,keyboard?XBOX_PROMPT_KEYBOARD:XBOX_PROMPT_PLAYSTATION,-1)==b);
  state=(XBOX_INPUT_STATE){0};if(keyboard)test_keys[b]=1;else state.Gamepad.bAnalogButtons[2]=255;
  mapper(&state,keyboard);assert(state.Gamepad.bAnalogButtons[2]==255 && !state.Gamepad.bAnalogButtons[0] && !state.Gamepad.bAnalogButtons[1]);
  reset_input();controls_assign(g_binds[device][MENU_CONTEXT],10,keyboard?'H':12);
  if(keyboard)test_keys['H']=1;else state.Gamepad.bAnalogButtons[4]=255;
  mapper(&state,keyboard);assert(state.Gamepad.bAnalogButtons[2]==255);
  assert(recomp_controls_prompt_binding(10,keyboard?XBOX_PROMPT_KEYBOARD:XBOX_PROMPT_PLAYSTATION,-1)==(keyboard?'H':12));
  g_binds[device][MENU_CONTEXT][10]=legacy_default_binding(device,MENU_CONTEXT,10);
 }
 /* Captive pickup uses the cinematic substate, sometimes with joystick
  * disabled. A held custom forward binding must resume without a release;
  * its fading Use icon must retain the binding, not the unbound menu Y. */
 for(int keyboard=0;keyboard<=1;++keyboard)for(unsigned disabled=0;disabled<2;++disabled){
  reset_input();play(4);recomp_controls_substate(0);
  unsigned b=keyboard?'T':8,device=keyboard?0:1;
  controls_assign(g_binds[device][3],19,b);
  controls_assign(g_binds[device][3],11,keyboard?'G':10);
  state=(XBOX_INPUT_STATE){0};if(keyboard)test_keys[b]=1;else state.Gamepad.bAnalogButtons[0]=255;
  mapper(&state,keyboard);assert(state.Gamepad.sThumbLY==32767);
  if(disabled){
   recomp_controls_state(0x4249D707,0xC2CBD863,15,0);
   assert(recomp_controls_prompt_binding(11,keyboard?XBOX_PROMPT_KEYBOARD:XBOX_PROMPT_PLAYSTATION,-1)==(keyboard?'G':10));
  }
  recomp_controls_state(0x4249D707,0x9F19476E,disabled?15:4,0);recomp_controls_substate(0);
  state=(XBOX_INPUT_STATE){0};if(!keyboard)state.Gamepad.bAnalogButtons[0]=255;
  mapper(&state,keyboard);assert(!state.Gamepad.sThumbLY);
  assert(recomp_controls_prompt_binding(11,keyboard?XBOX_PROMPT_KEYBOARD:XBOX_PROMPT_PLAYSTATION,-1)==(keyboard?'G':10));
  assert(recomp_controls_prompt_binding(8,keyboard?XBOX_PROMPT_KEYBOARD:XBOX_PROMPT_PLAYSTATION,-1)==resolved_binding(device,MENU_CONTEXT,8));
  play(4);recomp_controls_substate(0);state=(XBOX_INPUT_STATE){0};if(!keyboard)state.Gamepad.bAnalogButtons[0]=255;
  mapper(&state,keyboard);assert(state.Gamepad.sThumbLY==32767);
 }
 for(int d=0;d<2;++d)for(int ctx=0;ctx<CONTROL_CONTEXTS;++ctx)for(unsigned a=0;a<26;++a)
  g_binds[d][ctx][a]=legacy_default_binding(d,ctx,a);
 /* Rebound Space is Jump on foot (B), Ascend in a heli (A). Never dispatch
  * the already mapped B against the new vehicle table, and do not require
  * release of Space to begin ascending on the next correctly mapped poll. */
 reset_input();play(4);recomp_controls_substate(0);controls_assign(g_binds[0][3],9,VK_SPACE);
 controls_assign(g_binds[0][0],8,VK_SPACE);test_keys[VK_SPACE]=1;mapper(&state,TRUE);
 assert(state.Gamepad.bAnalogButtons[1]==255);
 assert(!recomp_controls_event_current(0x4249D707,0xC2CBD863,1,0));
 play(1);recomp_controls_substate(0);mapper(&state,TRUE);
 assert(state.Gamepad.bAnalogButtons[0]==255 && !state.Gamepad.bAnalogButtons[1]);
 assert(recomp_controls_event_current(0x4249D707,0xC2CBD863,1,0));
 /* Help/support, PDA, result screen and shell handoff use the same barrier. */
 for(unsigned modal=1;modal<=5;++modal){
  if(modal==3)continue;
  reset_input();play(4);recomp_controls_substate(modal);test_keys[VK_RETURN]=1;mapper(&state,TRUE);
  play(4);recomp_controls_substate(0);controls_assign(g_binds[0][3],14,VK_RETURN);mapper(&state,TRUE);
  assert(!state.Gamepad.bAnalogButtons[6]);
 }
 reset_input();play(12);recomp_controls_substate(0);controls_assign(g_binds[0][11],11,'V');test_keys['V']=1;
 mapper(&state,TRUE);assert(state.Gamepad.bAnalogButtons[3]==255);mapper(&state,TRUE);assert(state.Gamepad.bAnalogButtons[3]==255);
 play(4);recomp_controls_substate(0);mapper(&state,TRUE);assert(!state.Gamepad.bAnalogButtons[3]);
 test_keys['V']=0;mapper(&state,TRUE);test_keys['V']=1;mapper(&state,TRUE);assert(state.Gamepad.bAnalogButtons[3]==255);
 /* Upgrade the old editor's saved empty secondary slot once, then retain
  * an intentional unbind on later launches. */
 write_value("Controls","MenuBindingsVersion",1);
 for(int device=0;device<2;++device){g_binds[device][MENU_CONTEXT][10]=0xffff;save_binding(device,MENU_CONTEXT,10);g_binds[device][MENU_CONTEXT][11]=0xffff;save_binding(device,MENU_CONTEXT,11);g_binds[device][MENU_CONTEXT][15]=0xffff;save_binding(device,MENU_CONTEXT,15);}
 g_initialized=0;recomp_controls_init((HWND)1);
 assert(g_binds[0][MENU_CONTEXT][10]==VK_DELETE && g_binds[1][MENU_CONTEXT][10]==10);
 assert(g_binds[0][MENU_CONTEXT][11]==legacy_default_binding(0,3,11) && g_binds[1][MENU_CONTEXT][11]==11);
 assert(g_binds[0][MENU_CONTEXT][15]==legacy_default_binding(0,3,15) && g_binds[1][MENU_CONTEXT][15]==15);
 for(int device=0;device<2;++device){g_binds[device][MENU_CONTEXT][10]=0xffff;save_binding(device,MENU_CONTEXT,10);g_binds[device][MENU_CONTEXT][11]=0xffff;save_binding(device,MENU_CONTEXT,11);g_binds[device][MENU_CONTEXT][15]=0xffff;save_binding(device,MENU_CONTEXT,15);}
 g_initialized=0;recomp_controls_init((HWND)1);
 assert(g_binds[0][MENU_CONTEXT][10]==0xffff && g_binds[1][MENU_CONTEXT][10]==0xffff);
 assert(g_binds[0][MENU_CONTEXT][11]==0xffff && g_binds[1][MENU_CONTEXT][11]==0xffff);
 assert(g_binds[0][MENU_CONTEXT][15]==0xffff && g_binds[1][MENU_CONTEXT][15]==0xffff);

 /* Tutorial body describes gameplay while its footer accepts menu inputs.
  * Cover every exposed gameplay action and both device families, including
  * a Disabled joystick while the modal animation runs. */
 for(unsigned c=0;c<5;++c)for(int device=0;device<2;++device){
  reset_input();play(c+1);recomp_controls_substate(0);
  recomp_controls_state(0x4249D707,0xDDFB69D8,15,0);recomp_controls_substate(5);
  recomp_controls_prompt_gameplay_body(1);
  for(unsigned a=0;a<24;++a)if(action_available(c,a))
   assert(recomp_controls_prompt_binding(a,device?XBOX_PROMPT_PLAYSTATION:XBOX_PROMPT_KEYBOARD,-1)==resolved_binding(device,c,a));
  recomp_controls_prompt_gameplay_body(0);
  assert(recomp_controls_prompt_binding(8,device?XBOX_PROMPT_PLAYSTATION:XBOX_PROMPT_KEYBOARD,-1)==resolved_binding(device,MENU_CONTEXT,8));
 }
 /* Satellite Back adds to R3; it neither replaces the original binding nor
  * dispatches a shared Fire binding, and is consumed on camera exit. */
 reset_input();recomp_freecam_set(0);play(7);recomp_controls_substate(3);
 g_binds[0][MENU_CONTEXT][9]='B';g_binds[0][3][7]='M';g_binds[0][3][15]='B';
 test_keys['B']=1;mapper(&state,TRUE);assert((state.Gamepad.wButtons&0x80) && !state.Gamepad.bAnalogButtons[7]);
 play(4);mapper(&state,TRUE);assert(!(state.Gamepad.wButtons&0x80) && !state.Gamepad.bAnalogButtons[7]);
 reset_input();play(7);recomp_controls_substate(0);test_keys['M']=1;mapper(&state,TRUE);assert(state.Gamepad.wButtons&0x80);
 reset_input();g_binds[0][MENU_CONTEXT][9]=0xffffu;test_keys['M']=1;mapper(&state,TRUE);assert(state.Gamepad.wButtons&0x80);
 reset_input();g_binds[0][MENU_CONTEXT][9]='B';test_keys['B']=1;
 for(unsigned joy=1;joy<=14;++joy){if(joy==7)continue;play(joy);recomp_controls_substate(0);mapper(&state,TRUE);assert(!(state.Gamepad.wButtons&0x80));}
 reset_input();play(7);recomp_controls_substate(3);
 for(unsigned b=0;b<24;++b){
  reset_input();g_binds[1][MENU_CONTEXT][9]=b;
  for(unsigned a=0;a<CONTROL_ACTIONS;++a)g_binds[1][3][a]=a;
  float physical[24]={0};physical[b]=1;pack(&state.Gamepad,physical);mapper(&state,FALSE);
  assert(state.Gamepad.wButtons&0x80);if(b==15)assert(!state.Gamepad.bAnalogButtons[7]);
 }
 reset_input();play(7);recomp_controls_substate(0);g_binds[1][MENU_CONTEXT][9]=9;state=(XBOX_INPUT_STATE){0};state.Gamepad.wButtons=0x80;mapper(&state,FALSE);assert(state.Gamepad.wButtons&0x80);
 for(unsigned device=0;device<2;++device)for(unsigned a=0;a<CONTROL_ACTIONS;++a){g_binds[device][3][a]=legacy_default_binding(device,3,a);g_binds[device][MENU_CONTEXT][a]=legacy_default_binding(device,MENU_CONTEXT,a);}
 reset_input();play(4);

 /* Shared input policy applies only to future edits in this device/context. */
 reset_input();recomp_controls_open();
 assert(g_shared_inputs && strstr(recomp_controls_label(RECOMP_CONTROLS_ROW+4),"ON"));
 unsigned short original[2][CONTROL_CONTEXTS][CONTROL_ACTIONS];memcpy(original,g_binds,sizeof(original));
 for(int device=0;device<2;++device){
  g_device=device;g_edit_context=3;
  unsigned short jump=device?9:VK_SPACE,reload=device?8:'E';
  for(unsigned a=0;a<CONTROL_ACTIONS;++a)g_binds[device][3][a]=0xffffu;
  g_binds[device][3][9]=jump;g_binds[device][3][8]=reload;
  g_shared_inputs=0;g_listen=9;accept_binding(reload);g_release=0;
  assert(g_binds[device][3][9]==reload && g_binds[device][3][8]==jump);
  assert(GetPrivateProfileIntA(device?"Gamepad.4":"Keyboard.4","Action8",-1,g_path)==jump);
  g_shared_inputs=1;g_listen=8;accept_binding(reload);g_release=0;
  assert(g_binds[device][3][9]==reload && g_binds[device][3][8]==reload);
  g_shared_inputs=0;g_listen=8;accept_binding(0xffffu);g_release=0;
  assert(g_binds[device][3][9]==reload); /* Clearing never steals another bind. */
 }
 memcpy(g_binds,original,sizeof(original));
 g_device=0;g_edit_context=MENU_CONTEXT;g_shared_inputs=0;
 g_binds[0][3][19]='W';g_binds[0][MENU_CONTEXT][0]=BIND_INHERIT;g_binds[0][MENU_CONTEXT][8]=VK_RETURN;
 g_listen=8;accept_binding('W');g_release=0;
 assert(g_binds[0][MENU_CONTEXT][0]==VK_RETURN && g_binds[0][3][19]=='W');
 assert(g_binds[1][MENU_CONTEXT][0]==original[1][MENU_CONTEXT][0]);
 /* Existing duplicates move together on a subsequent conflicting assignment. */
 g_edit_context=3;g_binds[0][3][8]='E';g_binds[0][3][9]='E';g_binds[0][3][10]='C';
 g_listen=10;accept_binding('E');g_release=0;
 assert(g_binds[0][3][8]=='C' && g_binds[0][3][9]=='C' && g_binds[0][3][10]=='E');
 memcpy(g_binds,original,sizeof(original));
 recomp_controls_input(4,5,1);assert(g_shared_inputs);
 assert(GetPrivateProfileIntA("Controls","AllowSharedInputs",0,g_path)==1);
 recomp_controls_input(4,3,1);assert(!g_shared_inputs);
 g_initialized=0;recomp_controls_init((HWND)1);assert(!g_shared_inputs);
 recomp_controls_open();recomp_controls_input(4,5,1);assert(g_shared_inputs);
 memcpy(g_binds,original,sizeof(original));g_release=0;g_listen=-1;
 recomp_controls_close();
 /* Capture must accept a fresh key even with an unrelated key held when
  * the row was selected. The confirming Enter itself must not be captured. */
 reset_input();recomp_controls_open();g_menu=2;g_page=0;g_device=0;g_edit_context=3;
 test_keys[VK_RETURN]=test_keys[VK_SHIFT]=1;
 recomp_controls_input(2,5,1);state=(XBOX_INPUT_STATE){0};mapper(&state,TRUE);
 assert(g_listen==19);
 test_keys[VK_RETURN]=0;mapper(&state,TRUE);
 test_keys['L']=1;mapper(&state,TRUE);
 assert(g_listen<0 && g_binds[0][3][19]=='L');
 test_keys['L']=0;mapper(&state,TRUE);assert(!g_release);
 reset_input();recomp_controls_close();
 /* Main-menu car/on-foot capture tolerates pre-held mouse/keyboard inputs,
  * including persistent virtual-key states unrelated to navigation. */
 for(unsigned c=0;c<2;++c)for(unsigned h=0;h<4;++h){
  static const unsigned held[]={VK_SHIFT,VK_CONTROL,VK_LBUTTON,0xff};
  reset_input();recomp_controls_state(0x59C00449u,0,0,1);
  recomp_controls_open();g_menu=2;g_page=0;g_device=0;g_edit_context=c?3:1;
  unsigned action=action_at(0);test_keys[held[h]]=1;test_keys[VK_RETURN]=1;
  recomp_controls_input(2,5,1);test_keys[VK_RETURN]=0;
  test_keys['P']=1;mapper(&state,TRUE);
  assert(g_listen<0 && g_binds[0][g_edit_context][action]=='P');
 }
 /* A held key becomes eligible after its own release, even while another
  * initial key stays down. Mouse buttons follow the same rule. */
 reset_input();g_menu=2;g_page=0;g_device=0;g_edit_context=3;
 test_keys[VK_SHIFT]=test_keys[VK_CONTROL]=1;recomp_controls_input(2,5,1);
 test_keys[VK_SHIFT]=0;mapper(&state,TRUE);test_keys[VK_SHIFT]=1;mapper(&state,TRUE);
 assert(g_binds[0][3][19]==VK_SHIFT && g_listen<0);
 reset_input();g_menu=2;g_device=0;test_keys[VK_SHIFT]=1;recomp_controls_input(2,5,1);
 test_keys[VK_RBUTTON]=1;mapper(&state,TRUE);assert(g_listen<0 && g_binds[0][3][19]==VK_RBUTTON);
 /* Controller capture also accepts a new button while an old axis stays held. */
 reset_input();g_menu=2;g_device=1;recomp_controls_input(2,5,1);
 state=(XBOX_INPUT_STATE){0};state.Gamepad.bAnalogButtons[0]=255;state.Gamepad.sThumbRX=30000;
 mapper(&state,FALSE);assert(g_listen==19);
 state=(XBOX_INPUT_STATE){0};state.Gamepad.bAnalogButtons[1]=255;state.Gamepad.sThumbRX=30000;
 mapper(&state,FALSE);assert(g_listen<0 && g_binds[1][3][19]==9);
 /* Regaining focus snapshots held keys instead of binding an Alt-Tab key. */
 reset_input();g_device=0;g_menu=2;recomp_controls_input(2,5,1);
 test_focus=(HWND)2;mapper(&state,TRUE);test_keys['T']=1;
 test_focus=(HWND)1;mapper(&state,TRUE);assert(g_listen==19);
 test_keys['P']=1;mapper(&state,TRUE);assert(g_listen<0 && g_binds[0][3][19]=='P');
 reset_input();recomp_controls_close();play(4);
 /* Binding release must follow the captured input, not every held input.
  * A modifier/controller axis held afterwards used to lock both devices. */
 memcpy(original,g_binds,sizeof(original));
 reset_input();recomp_controls_open();g_menu=2;g_device=0;g_edit_context=3;
 g_listen=19;g_armed=1;test_keys['L']=1;test_keys[VK_MENU]=1;
 state=(XBOX_INPUT_STATE){0};mapper(&state,TRUE);assert(g_listen<0 && g_release);
 test_keys['L']=0;mapper(&state,TRUE);assert(!g_release);
 g_binds[0][MENU_CONTEXT][0]='W';test_keys['W']=1;mapper(&state,TRUE);assert(state.Gamepad.wButtons&1);
 reset_input();g_device=1;g_listen=19;g_armed=1;
 state=(XBOX_INPUT_STATE){0};state.Gamepad.bAnalogButtons[0]=255;state.Gamepad.sThumbRX=30000;
 mapper(&state,FALSE);assert(g_release && g_listen<0);
 state=(XBOX_INPUT_STATE){0};state.Gamepad.sThumbRX=30000;mapper(&state,FALSE);assert(!g_release);
 /* A held controller cannot prevent keyboard capture from arming, and
  * reserved modifiers are not candidate keyboard bindings. */
 reset_input();g_device=0;g_listen=19;test_keys[VK_MENU]=1;
 state=(XBOX_INPUT_STATE){0};state.Gamepad.sThumbRX=30000;mapper(&state,FALSE);
 mapper(&state,TRUE);assert(g_armed && g_listen==19);
 test_keys['L']=1;mapper(&state,TRUE);assert(g_release && g_listen<0);
 test_keys['L']=test_keys[VK_MENU]=0;mapper(&state,TRUE);assert(!g_release);
 reset_input();g_device=1;
 /* F10 recovers even if the captured controller stops being polled. */
 g_listen=19;accept_binding(8);assert(g_release);
 recomp_controls_message((HWND)1,WM_KEYDOWN,VK_F10,0);mapper(&state,TRUE);assert(!recomp_controls_is_binding());
 assert(g_binds[1][3][19]==8);
 /* Clearing a pad binding is a keyboard operation; no pad release required. */
 g_listen=19;test_keys[VK_BACK]=1;
 recomp_controls_message((HWND)1,WM_KEYDOWN,VK_BACK,0);assert(g_release);
 test_keys[VK_BACK]=0;mapper(&state,TRUE);assert(!g_release);
 memcpy(g_binds,original,sizeof(original));reset_input();recomp_controls_close();
 test_vehicle_contexts();
 test_free_camera();
 test_free_camera_roll();
 puts("controls: all production input, persistence, menu, focus and mouse tests passed");return 0;
}
"""
dev_source=(ROOT/'ports/mercenaries/src/dev_menu.c').read_text()
toggle=dev_source[dev_source.index('void recomp_dev_freecam_toggle(void)'):dev_source.index('static void mission_choices(void)')]
host=(ROOT/'ports/mercenaries/src/main.c').read_text()
handler=host[host.index('        if (wparam == VK_F11'):host.index('        if (wparam == VK_F9')]
source=source.replace('/*DEVELOPER_HOTKEY*/',toggle+'\nstatic int test_developer_hotkey(WPARAM wparam, LPARAM lparam){'+handler+'return 0;}')
with tempfile.TemporaryDirectory(prefix='merc-controls-') as folder:
 d=Path(folder);(d/'test.c').write_text(source,encoding='utf-8')
 exe=d/'test.exe'
 command=['C:/MinGW/bin/gcc.exe','-std=c11','-D_WIN32_WINNT=0x0601','-O2','-I'+str(ROOT/'ports/mercenaries/src'),'-I'+str(ROOT/'src/input'),'-I'+str(ROOT/'src'),'-I'+str(ROOT/'include'),str(d/'test.c'),'-o',str(exe),'-luser32','-lm']
 subprocess.run(command,check=True)
 subprocess.run([str(exe),str(d)],check=True)

