"""Exercise real Windows input entry points with counted, fake host devices."""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[2]


class InputIsolationTests(unittest.TestCase):
    def test_no_physical_input_or_feedback_in_isolated_mode(self):
        source=(ROOT/'src/input/xinput_device.c').read_text(encoding='utf-8')
        activity=source[source.index('#include "prompt_activity.h"'):source.index('static XBOX_INPUT_MAPPER')].replace('"prompt_activity.h"','"input/prompt_activity.h"')
        overlay=source[source.index("#ifdef _WIN32\nstatic volatile LONG g_overlay_input_capture"):source.index("/* Input is polled") ]
        cache=source[source.index('#define INPUT_ENV_CACHE_CAPACITY'):
                     source.index('void xbox_InputStopTestAutoA')]
        names=['keyboard_fallback_enabled','test_input_isolated','key_down',
               'xbox_InputInit','input_get_state','xbox_InputGetState','xbox_InputGetOverlayState','xbox_InputSetState',
               'xbox_InputIsConnected','xbox_InputGetCapabilities']
        bodies=[re.search(r'(?:static )?(?:BOOL|void|DWORD) '+name+r'\([^;{}]*\)\s*\n\{.*?\n\}',source,re.S)[0] for name in names]
        globals=source[source.index('static BOOL  g_controller_connected'):source.index('void xbox_InputArmTestAutoY')]
        prelude=r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <math.h>
#include <windows.h>
#include "input/xinput_xbox.h"
/* The bundled MinGW headers predate XInput; these are the SDK ABI fields
 * touched by the production mapper, with every host operation stubbed below. */
typedef struct {WORD wButtons;BYTE bLeftTrigger,bRightTrigger;SHORT sThumbLX,sThumbLY,sThumbRX,sThumbRY;} XINPUT_GAMEPAD;
typedef struct {DWORD dwPacketNumber;XINPUT_GAMEPAD Gamepad;} XINPUT_STATE;
typedef struct {WORD wLeftMotorSpeed,wRightMotorSpeed;} XINPUT_VIBRATION;
typedef struct {BYTE Type,SubType;WORD Flags;XINPUT_GAMEPAD Gamepad;XINPUT_VIBRATION Vibration;} XINPUT_CAPABILITIES;
#define XINPUT_GAMEPAD_A 0x1000
#define XINPUT_GAMEPAD_B 0x2000
#define XINPUT_GAMEPAD_X 0x4000
#define XINPUT_GAMEPAD_Y 0x8000
#define XINPUT_GAMEPAD_LEFT_SHOULDER 0x100
#define XINPUT_GAMEPAD_RIGHT_SHOULDER 0x200
static unsigned host_calls,keyboard_polls;
static XINPUT_STATE host_state={123,{XINPUT_GAMEPAD_A,0,0,0,0,0,0}};
static XINPUT_VIBRATION last_vibration;
static int host_connected=1,sdl_connected;
static XBOX_INPUT_STATE test_sdl_state;
static int held_key=1;
static int specific_key=-1;
static const char *configuration;
static BOOL g_test_auto_a_stopped;
static XBOX_INPUT_MAPPER g_input_mapper;
static const char *fake_getenv(const char *name){
 return !strcmp(name,"MERCENARIES_TEST_ISOLATE_INPUT")?configuration:NULL;
}
#define getenv fake_getenv
static DWORD fake_xi_get(DWORD port,XINPUT_STATE *s){
 ++host_calls;*s=host_state;return host_connected?ERROR_SUCCESS:ERROR_DEVICE_NOT_CONNECTED;
}
static DWORD fake_xi_set(DWORD port,XINPUT_VIBRATION *v){++host_calls;last_vibration=*v;return ERROR_SUCCESS;}
static DWORD fake_xi_caps(DWORD port,DWORD flags,XINPUT_CAPABILITIES *c){
 ++host_calls;memset(c,0,sizeof(*c));c->Type=1;c->SubType=1;return ERROR_SUCCESS;
}
static void dualsense_sdl_init(BOOL *connected){++host_calls;}
static BOOL dualsense_sdl_is_connected(DWORD port){++host_calls;return FALSE;}
static DWORD dualsense_sdl_get_state(DWORD port,XBOX_INPUT_STATE *s){++host_calls;*s=test_sdl_state;return sdl_connected&&port==0?ERROR_SUCCESS:ERROR_DEVICE_NOT_CONNECTED;}
static DWORD dualsense_sdl_set_vibration(DWORD port,const XBOX_VIBRATION *v){++host_calls;return ERROR_SUCCESS;}
static void read_keyboard_state(XBOX_INPUT_STATE *s){
 ++keyboard_polls;memset(s,0,sizeof(*s));s->Gamepad.bAnalogButtons[XBOX_BUTTON_A]=123;
}
static HWND fake_foreground(void){++host_calls;return (HWND)1;}
static DWORD fake_foreground_pid(HWND w,DWORD *p){++host_calls;*p=42;return 1;}
static DWORD fake_pid(void){return 42;}
static SHORT fake_key(int key){++host_calls;return held_key && (specific_key<0 || specific_key==key)?(SHORT)0x8000:0;}
static ULONGLONG fake_tick(void){return 1000;}
#define XInputGetState fake_xi_get
#define XInputSetState fake_xi_set
#define XInputGetCapabilities fake_xi_caps
#define GetForegroundWindow fake_foreground
#define GetWindowThreadProcessId fake_foreground_pid
#define GetCurrentProcessId fake_pid
#define GetAsyncKeyState fake_key
#define GetTickCount64 fake_tick
'''
        tail=r'''
int main(int argc,char **argv){
 int mode=atoi(argv[1]);configuration=mode==0?NULL:mode==1?"":mode==2?"0":"1";
 int isolated=mode==3;XBOX_INPUT_STATE s;XBOX_INPUT_CAPABILITIES c;XBOX_VIBRATION v={65535,65535};
 xbox_InputInit();assert((host_calls==0)==isolated);
 assert(key_down('Z')==!isolated);
 assert(xbox_InputGetState(0,&s)==ERROR_SUCCESS);
 assert(s.Gamepad.bAnalogButtons[XBOX_BUTTON_A]==(isolated?123:255));
 assert(keyboard_polls==1);
 assert(xbox_InputIsConnected(0));
 assert(xbox_InputGetCapabilities(0,0,&c)==ERROR_SUCCESS && c.Type==1 && c.SubType==1);
 assert(xbox_InputSetState(0,&v)==ERROR_SUCCESS);
 for(DWORD port=1;port<4;port++){
  assert(xbox_InputGetState(port,&s)==(isolated?ERROR_DEVICE_NOT_CONNECTED:ERROR_SUCCESS));
  assert(xbox_InputIsConnected(port)==!isolated);
  assert(xbox_InputGetCapabilities(port,0,&c)==(isolated?ERROR_DEVICE_NOT_CONNECTED:ERROR_SUCCESS));
  assert(xbox_InputSetState(port,&v)==(isolated?ERROR_DEVICE_NOT_CONNECTED:ERROR_SUCCESS));
 }
 assert(xbox_InputGetState(4,&s)==ERROR_DEVICE_NOT_CONNECTED);
 assert(xbox_InputGetState(0,NULL)==ERROR_DEVICE_NOT_CONNECTED);
 assert(xbox_InputGetCapabilities(0,0,NULL)==ERROR_DEVICE_NOT_CONNECTED);
 assert(xbox_InputSetState(0,NULL)==ERROR_DEVICE_NOT_CONNECTED);
 if(!isolated){
  /* Exercise each host control independently on port one, excluding keyboard
   * fallback. All calls run the production Windows controller mapper. */
  const WORD masks[]={0x1000,0x2000,0x4000,0x8000,0x0200,0x0100};
  const unsigned analog[]={XBOX_BUTTON_A,XBOX_BUTTON_B,XBOX_BUTTON_X,XBOX_BUTTON_Y,XBOX_BUTTON_BLACK,XBOX_BUTTON_WHITE};
  memset(&host_state,0,sizeof(host_state));
  for(unsigned i=0;i<6;i++){
   host_state.Gamepad.wButtons=masks[i];
   assert(xbox_InputGetState(1,&s)==ERROR_SUCCESS);
   assert(s.Gamepad.wButtons==0);
   for(unsigned j=0;j<8;j++)assert(s.Gamepad.bAnalogButtons[j]==(j==analog[i]?255:0));
  }
  for(unsigned bit=0;bit<8;bit++){
   host_state.Gamepad.wButtons=(WORD)(1u<<bit);assert(xbox_InputGetState(1,&s)==ERROR_SUCCESS);
   assert(s.Gamepad.wButtons==(1u<<bit));
   for(unsigned j=0;j<8;j++)assert(s.Gamepad.bAnalogButtons[j]==0);
  }
  host_state.Gamepad.wButtons=0;host_state.dwPacketNumber=456;
  host_state.Gamepad.bLeftTrigger=73;host_state.Gamepad.bRightTrigger=219;
  host_state.Gamepad.sThumbLX=11000;host_state.Gamepad.sThumbLY=-13000;
  host_state.Gamepad.sThumbRX=22000;host_state.Gamepad.sThumbRY=-17000;
  assert(xbox_InputGetState(1,&s)==ERROR_SUCCESS && s.dwPacketNumber==456);
  assert(s.Gamepad.bAnalogButtons[XBOX_BUTTON_LTRIGGER]==73 && s.Gamepad.bAnalogButtons[XBOX_BUTTON_RTRIGGER]==219);
  assert(s.Gamepad.sThumbLX==11000 && s.Gamepad.sThumbLY==-13000 && s.Gamepad.sThumbRX==22000 && s.Gamepad.sThumbRY==-17000);
  v.wLeftMotorSpeed=12345;v.wRightMotorSpeed=54321;assert(xbox_InputSetState(1,&v)==ERROR_SUCCESS);
  assert(last_vibration.wLeftMotorSpeed==12345 && last_vibration.wRightMotorSpeed==54321);
  host_connected=0;assert(xbox_InputGetState(1,&s)==ERROR_DEVICE_NOT_CONNECTED && !xbox_InputIsConnected(1));
  host_connected=1;assert(xbox_InputGetState(1,&s)==ERROR_SUCCESS && xbox_InputIsConnected(1));
  puts("PASS: XInput buttons, D-pad, start/back, stick clicks, axes, triggers, packet, rumble and reconnect");
 }
 if(isolated)assert(host_calls==0);else assert(host_calls>0);
 unsigned before=host_calls;
 xbox_InputSetOverlayCapture(TRUE);
 memset(&s,0xFF,sizeof(s));assert(xbox_InputGetState(0,&s)==ERROR_SUCCESS);
 XBOX_INPUT_STATE empty={0};assert(!memcmp(&s,&empty,sizeof(s)));assert(host_calls==before);
 /* A separate host-camera read may sample mapped devices without releasing
  * the capture gate on the packet destined for retail. */
 assert(xbox_InputGetOverlayState(0,&s)==ERROR_SUCCESS);
 if(isolated)assert(host_calls==0);else assert(host_calls>before);
 before=host_calls;
 memset(&s,0xFF,sizeof(s));assert(xbox_InputGetState(0,&s)==ERROR_SUCCESS);
 assert(!memcmp(&s,&empty,sizeof(s)) && host_calls==before);
 xbox_InputSetOverlayCapture(FALSE);held_key=1;
 assert(xbox_InputGetState(0,&s)==ERROR_SUCCESS);
 /* A held close press must not zero a connected gamepad. Keyboard fallback
  * here is a controlled dependency; actual per-key filtering is checked below. */
 assert(s.Gamepad.bAnalogButtons[XBOX_BUTTON_A]==123);
 if(isolated)assert(host_calls==0);
 else {
  assert(!xbox_InputKeyDown(VK_F9));
  assert(!xbox_InputKeyDown(VK_ESCAPE));
  assert(!xbox_InputKeyDown(VK_RETURN));
  assert(!xbox_InputKeyDown(VK_LBUTTON));
  assert(xbox_InputKeyDown('W'));
 }
 held_key=0;assert(xbox_InputGetState(0,&s)==ERROR_SUCCESS);
 assert(s.Gamepad.bAnalogButtons[XBOX_BUTTON_A]==123);
 if(!isolated){
  /* Every virtual key, including the Windows gamepad range, may remain held
   * without suppressing the controller after the panel closes. */
  host_state.Gamepad.wButtons=XINPUT_GAMEPAD_A;
  for(int key=1;key<256;++key){
   specific_key=key;held_key=1;
   xbox_InputSetOverlayCapture(TRUE);xbox_InputSetOverlayCapture(FALSE);
   for(int poll=0;poll<5;++poll){
    assert(xbox_InputGetState(1,&s)==ERROR_SUCCESS);
    assert(s.Gamepad.bAnalogButtons[XBOX_BUTTON_A]==255);
   }
   int closing=key==VK_F9 || key==VK_ESCAPE || key==VK_RETURN || key==VK_LBUTTON;
   assert(xbox_InputKeyDown(key)==!closing);
   assert(xbox_InputKeyDownContinuous(key));
   held_key=0;assert(xbox_InputGetState(1,&s)==ERROR_SUCCESS);
   held_key=1;assert(xbox_InputKeyDown(key));
  }
  /* Reopening must capture again even during an unfinished close press. */
  xbox_InputSetOverlayCapture(TRUE);
  assert(xbox_InputGetState(1,&s)==ERROR_SUCCESS && !memcmp(&s,&empty,sizeof(s)));
  assert(!xbox_InputKeyDown(specific_key));
  assert(!xbox_InputKeyDownContinuous(specific_key));
  held_key=0;xbox_InputSetOverlayCapture(FALSE);
  assert(!xbox_InputKeyDown(0)&&!xbox_InputKeyDown(256)&&!xbox_InputKeyDown(-1));
 }
 if(!isolated){
  memset(&host_state,0,sizeof(host_state));xbox_InputGetState(0,&s);
  xbox_InputNotifyKeyboardActivity();assert(xbox_InputPromptDevice()==XBOX_PROMPT_KEYBOARD);
  host_state.Gamepad.wButtons=XINPUT_GAMEPAD_Y;xbox_InputGetState(0,&s);assert(xbox_InputPromptDevice()==XBOX_PROMPT_XBOX);
  xbox_InputNotifyKeyboardActivity();xbox_InputGetState(0,&s);assert(xbox_InputPromptDevice()==XBOX_PROMPT_KEYBOARD);
  sdl_connected=1;test_sdl_state.Gamepad.sThumbLX=24000;xbox_InputGetState(0,&s);assert(xbox_InputPromptDevice()==XBOX_PROMPT_PLAYSTATION);
  xbox_InputNotifyKeyboardActivity();xbox_InputGetState(0,&s);assert(xbox_InputPromptDevice()==XBOX_PROMPT_KEYBOARD);
  test_sdl_state.Gamepad.sThumbLX=-24000;xbox_InputGetState(0,&s);assert(xbox_InputPromptDevice()==XBOX_PROMPT_PLAYSTATION);
  host_state.Gamepad.wButtons=XINPUT_GAMEPAD_B;xbox_InputGetState(0,&s);assert(xbox_InputPromptDevice()==XBOX_PROMPT_XBOX);
  sdl_connected=0;host_connected=0;xbox_InputGetState(0,&s);
  xbox_InputNotifyKeyboardActivity();host_connected=1;xbox_InputGetState(0,&s);assert(xbox_InputPromptDevice()==XBOX_PROMPT_XBOX);
  puts("PASS: production device selection switches on real button/stick events, respects keyboard activity and reconnects");
 }
 puts("PASS: panel captures input; closing consumes only its held keys, restores gamepads immediately, and permits a fresh press after release");
 printf("mode=%d host_calls=%u scripted_polls=%u\n",mode,host_calls,keyboard_polls);
 return 0;
}
'''
        launcher=(ROOT/'tools/recomp/Run-HiddenRetailRoute.ps1').read_text(encoding='utf-8-sig')
        self.assertIn("MERCENARIES_TEST_ISOLATE_INPUT = '1'",launcher)
        for suffix in ('START_DELAY','A1_DELAY','A2_DELAY','A3_DELAY','PULSE'):
            key='MERCENARIES_TEST_EXACT_'+suffix+'_MS'
            self.assertIn(key,source);self.assertIn(key,launcher)
        self.assertIn('MERCENARIES_TEST_EXACT_LOAD_GAME_SEQUENCE', source)
        self.assertIn('45000u, 65000u, 70000u, 75000u', source)
        self.assertNotIn('45000u, 65000u, 70000u, 78000u', source)
        self.assertIn("[switch]$LoadSave", launcher)
        self.assertIn("MERCENARIES_TEST_EXACT_LOAD_GAME_SEQUENCE = '1'", launcher)
        self.assertIn("$settings.Remove('MERCENARIES_TEST_EXACT_NEW_GAME_SEQUENCE')",
                      launcher)
        with tempfile.TemporaryDirectory(prefix='merc-input-isolation-') as directory:
            path=Path(directory);c=path/'test.c';exe=path/'test.exe'
            c.write_text(prelude+activity+overlay+cache+globals+'\n'.join(bodies)+tail,encoding='utf-8')
            build=subprocess.run([shutil.which('gcc') or 'C:/MinGW/bin/gcc.exe','-std=c11','-O2','-I',str(ROOT/'src'),str(c),'-o',str(exe)],capture_output=True)
            self.assertEqual(build.returncode,0,build.stderr.decode(errors='replace'))
            for mode in range(4):
                run=subprocess.run([str(exe),str(mode)],capture_output=True)
                self.assertEqual(run.returncode,0,run.stderr.decode(errors='replace'))
                print(run.stdout.decode().strip())


if __name__=='__main__':unittest.main()
