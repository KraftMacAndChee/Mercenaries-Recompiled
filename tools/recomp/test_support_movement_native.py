"""Support selection retains gameplay axes; full menus and add-item UI do not."""
from pathlib import Path
import ast, subprocess, tempfile
ROOT=Path(__file__).resolve().parents[2]
module=ast.parse((ROOT/'tools/recomp/test_controls_native.py').read_text())
source=next(ast.literal_eval(n.value) for n in module.body if isinstance(n,ast.Assign) and any(isinstance(t,ast.Name) and t.id=='source' for t in n.targets))
source=source[:source.index('static void test_free_camera(void)')].replace('/*DEVELOPER_HOTKEY*/','')
source+=r"""
static void support(unsigned substate,unsigned screen){
 reset_input();play(4);recomp_controls_state(0x4249D707,screen,4,1);recomp_controls_substate(substate);
}
int main(int argc,char**argv){
 assert(argc==2);g_xbox_mem_offset=(ptrdiff_t)camera_memory-0x10000;
 snprintf(test_ini,sizeof(test_ini),"%s\\runtime.exe",argv[1]);recomp_controls_init((HWND)1);
 XBOX_INPUT_STATE state;
 /* Default sticks stay analog and never create synthetic D-pad navigation. */
 for(unsigned screen=0;screen<2;++screen)for(unsigned a=16;a<24;++a){
  support(1,screen?0xC739FD0F:0xC2CBD863);float raw[24]={0},out[24];raw[a]=.73f;
  state=(XBOX_INPUT_STATE){0};pack(&state.Gamepad,raw);assert(mapper(&state,FALSE));unpack(&state.Gamepad,out);
  for(unsigned b=0;b<24;++b)assert(fabsf(out[b]-(a==b?.73f:0))<.0001f);
 }
 /* Default support cycling is still D-pad on controller, MB5/4 on keyboard. */
 for(int keyboard=0;keyboard<2;++keyboard)for(unsigned a=0;a<2;++a){
  support(1,0xC2CBD863);state=(XBOX_INPUT_STATE){0};
  if(keyboard)test_keys[a?VK_XBUTTON1:VK_XBUTTON2]=1;else state.Gamepad.wButtons=1u<<a;
  mapper(&state,keyboard);assert(state.Gamepad.wButtons==(1u<<a) && !state.Gamepad.sThumbLY);
  assert(recomp_controls_prompt_binding(a,keyboard?XBOX_PROMPT_KEYBOARD:XBOX_PROMPT_XBOX,-1)==(keyboard?(a?VK_XBUTTON1:VK_XBUTTON2):a));
 }
 support(1,0xC2CBD863);test_keys['W']=1;state=(XBOX_INPUT_STATE){0};mapper(&state,TRUE);
 assert(!state.Gamepad.wButtons && state.Gamepad.sThumbLY==32767);
 /* Rebound movement from right stick/button/key follows gameplay mapping. */
 for(int keyboard=0;keyboard<2;++keyboard){
  unsigned b=keyboard?'T':8;g_binds[keyboard?0:1][3][19]=b;
  support(1,0xC2CBD863);state=(XBOX_INPUT_STATE){0};
  if(keyboard)test_keys[b]=1;else state.Gamepad.bAnalogButtons[0]=255;
  mapper(&state,keyboard);assert(state.Gamepad.sThumbLY==32767);
  g_binds[keyboard?0:1][3][15]=b;play(4);recomp_controls_substate(0);
  state=(XBOX_INPUT_STATE){0};if(!keyboard)state.Gamepad.bAnalogButtons[0]=255;
  mapper(&state,keyboard);assert(state.Gamepad.sThumbLY==32767 && !state.Gamepad.bAnalogButtons[7]);
  for(unsigned a=0;a<26;++a)g_binds[keyboard?0:1][3][a]=default_binding(keyboard?0:1,3,a);
 }
 /* Explicit Menu direction binding stays valid, without overwriting saves. */
 g_binds[0][MENU_CONTEXT][0]='U';support(1,0xC2CBD863);test_keys['U']=1;state=(XBOX_INPUT_STATE){0};mapper(&state,TRUE);
 assert((state.Gamepad.wButtons&1) && !state.Gamepad.sThumbLY);g_binds[0][MENU_CONTEXT][0]=BIND_INHERIT;
 /* Help, pause, result, disconnected-controller and add animations stay modal.
  * HVT may restore Default substate while still on the support-add screen. */
 for(unsigned mode=0;mode<7;++mode){
  support(mode==0?0:mode==1?2:1,mode<2?0xC739FD0F:0xC2CBD863);
  if(mode==2){recomp_controls_state(0x4249D707,0x7084D38D,4,0);recomp_controls_substate(1);}
  if(mode==3){recomp_controls_state(0x4249D707,0xDDFB69D8,4,1);recomp_controls_substate(5);}
  if(mode==4){recomp_controls_state(0x4249D707,0x65872ADE,15,0);recomp_controls_substate(1);}
  if(mode==5)g_waiting_controller=1;
  if(mode==6)recomp_controls_open();
  assert(!support_movement_active());state=(XBOX_INPUT_STATE){0};state.Gamepad.sThumbLY=32767;
  mapper(&state,FALSE);assert(!state.Gamepad.sThumbLY);
  if(mode<2){state=(XBOX_INPUT_STATE){0};state.Gamepad.bAnalogButtons[0]=255;mapper(&state,FALSE);assert(state.Gamepad.bAnalogButtons[0]==255);}
  g_waiting_controller=0;recomp_controls_close();
 }
 puts("PASS: support movement and analog magnitude; D-pad/keyboard selection; rebound controls; held-fire barrier; full-menu and HVT exclusions");
}
"""
with tempfile.TemporaryDirectory(prefix='support-move-') as folder:
 d=Path(folder);(d/'test.c').write_text(source);exe=d/'test.exe'
 subprocess.run(['C:/MinGW/bin/gcc.exe','-std=c11','-D_WIN32_WINNT=0x0601','-O2',*['-I'+str(ROOT/p) for p in ['ports/mercenaries/src','src/input','src','include']],str(d/'test.c'),'-o',str(exe),'-luser32','-lm'],check=True)
 subprocess.run([str(exe),str(d)],check=True)
