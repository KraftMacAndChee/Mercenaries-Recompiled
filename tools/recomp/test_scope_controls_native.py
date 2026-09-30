"""Shared scope bindings: real input mapper, prompts, editor and INI migration."""
from pathlib import Path
import ast, subprocess, tempfile
ROOT=Path(__file__).resolve().parents[2]
module=ast.parse((ROOT/'tools/recomp/test_controls_native.py').read_text())
source=next(ast.literal_eval(n.value) for n in module.body if isinstance(n,ast.Assign) and any(isinstance(t,ast.Name) and t.id=='source' for t in n.targets))
source=source[:source.index('int main(')]
source+=r"""
int main(int argc,char**argv){
 assert(argc==2);g_xbox_mem_offset=(ptrdiff_t)camera_memory-0x10000;
 snprintf(test_ini,sizeof(test_ini),"%s\\runtime.exe",argv[1]);recomp_controls_init((HWND)1);
 assert(controls_context(5)==4 && controls_context(10)==4 && controls_binding_context(9)==4);
 assert(controls_context(9)==3 && controls_context(13)==3);
 assert(!strcmp(contexts[4],"SNIPER ZOOM") && contexts[9]==NULL);
 const unsigned actions[]={24,25,23,22,20,21,15,8,7,6,12,13,2,3,4,5};
 unsigned mask=0;for(unsigned i=0;i<16;++i)mask|=1u<<actions[i];
 g_edit_context=4;assert(action_count()==16 && page_count()==4);
 for(unsigned page=0;page<4;++page){g_page=page;for(unsigned row=0;row<4;++row)assert(action_at(row)==actions[page*4+row]);}
 assert(!action_available(3,24) && !action_available(3,25));
 for(unsigned a=0;a<26;++a)assert(action_available(4,a)==!!(mask&(1u<<a)));
 for(int d=0;d<2;++d)for(unsigned a=0;a<26;++a)
  if(mask&(1u<<a))assert(g_binds[d][4][a]==default_binding(d,4,a));
 assert(g_binds[0][4][24]==WHEEL_UP && g_binds[0][4][25]==WHEEL_DOWN);
 assert(g_binds[1][4][24]==19 && g_binds[1][4][25]==18);
 /* Every exposed action accepts every controller source in both scopes.
  * Left/right movement, jump, Use and grenade fire never leak through. */
 for(unsigned joy=5;joy<=10;joy+=5)for(unsigned a=0;a<26;++a)for(unsigned src=0;src<24;++src){
  reset_input();play(joy);for(unsigned b=0;b<26;++b)g_binds[1][4][b]=0xffff;
  g_binds[1][4][a]=src;float input[24]={0},output[24];input[src]=.75f;
  XBOX_INPUT_STATE state={0};pack(&state.Gamepad,input);assert(mapper(&state,FALSE));unpack(&state.Gamepad,output);
  int valid=(mask&(1u<<a)) && !(joy==5 && a==13);unsigned dest=a==24?19:a==25?18:a;
  for(unsigned b=0;b<24;++b)assert((output[b]>.5f)==(valid && b==dest));
  if(valid)for(int family=XBOX_PROMPT_XBOX;family<=XBOX_PROMPT_PLAYSTATION;++family)
   assert(recomp_controls_prompt_binding(a>=24?dest:a,family,-1)==src);
 }
 /* Independent keyboard scope controls, independent On Foot controls. */
 for(unsigned joy=5;joy<=10;joy+=5)for(unsigned a=0;a<26;++a){
  reset_input();play(joy);for(unsigned b=0;b<26;++b)g_binds[0][4][b]=0xffff;
  g_binds[0][4][a]='T';test_keys['T']=1;XBOX_INPUT_STATE state={0};assert(mapper(&state,TRUE));float out[24];unpack(&state.Gamepad,out);
  int valid=(mask&(1u<<a)) && !(joy==5 && a==13);unsigned dest=a==24?19:a==25?18:a;
  for(unsigned b=0;b<24;++b)assert((out[b]>.5f)==(valid && b==dest));
 }
 /* Each wheel detent is 4x once, in either direction, in either scope.
  * The default controller stick and rebound keyboard keys remain 1x. */
 for(int d=0;d<2;++d)for(unsigned a=0;a<26;++a)g_binds[d][4][a]=default_binding(d,4,a);
 for(unsigned joy=5;joy<=10;joy+=5)for(int sign=-1;sign<=1;sign+=2){
  reset_input();g_wheel_release=0;g_wheel_pending=sign;play(joy);g_mouse=0;
  XBOX_INPUT_STATE state={0};mapper(&state,TRUE);assert(state.Gamepad.sThumbLY==sign*32767);
  assert(recomp_controls_mouse_axis(0xD4F1922B,(float)sign,.016f)==sign*4.f);
  assert(recomp_controls_mouse_axis(0xD4F1922B,(float)sign,.016f)==sign);
  play(joy);state=(XBOX_INPUT_STATE){0};mapper(&state,TRUE);assert(!state.Gamepad.sThumbLY);
  state.Gamepad.sThumbLY=sign*20000;mapper(&state,FALSE);assert(abs(state.Gamepad.sThumbLY-sign*20000)<=1);
  assert(recomp_controls_mouse_axis(0xD4F1922B,(float)sign,.016f)==sign);
 }
 /* Import the effective old bindings, ignoring obsolete hidden scope maps. */
 for(int d=0;d<2;++d){
  for(unsigned a=0;a<26;++a){g_binds[d][3][a]=default_binding(d,3,a);g_binds[d][4][a]=d?9:'O';save_binding(d,4,a);}
  g_binds[d][3][7]=d?14:'H';g_binds[d][3][15]=0xffff;g_binds[d][3][24]=d?23:WHEEL_DOWN;
  for(unsigned a=0;a<26;++a)save_binding(d,3,a);
 }
 write_value("Controls","ScopeBindingsVersion",0);g_initialized=0;recomp_controls_init((HWND)1);
 for(int d=0;d<2;++d){
  assert(g_binds[d][4][7]==(d?14:'H') && g_binds[d][4][15]==0xffff && g_binds[d][4][24]==(d?23:WHEEL_DOWN));
  g_device=d;g_edit_context=4;g_listen=7;accept_binding(d?8:'J');g_listen=24;accept_binding(0xffff);
 }
 g_initialized=0;recomp_controls_init((HWND)1);
 for(int d=0;d<2;++d)assert(g_binds[d][4][7]==(d?8:'J') && g_binds[d][4][24]==0xffff && g_binds[d][3][7]==(d?14:'H'));
 /* Scope tutorial magnification resolves even with a Disabled live joystick. */
 reset_input();play(10);g_binds[0][4][24]=WHEEL_UP;
 recomp_controls_state(0x4249D707,0xDDFB69D8,15,0);recomp_controls_substate(5);recomp_controls_prompt_gameplay_body(1);
 assert(recomp_controls_prompt_binding(19,XBOX_PROMPT_KEYBOARD,-1)==WHEEL_UP);
 assert(recomp_controls_prompt_binding(19,XBOX_PROMPT_KEYBOARD,9)==WHEEL_UP);
 recomp_controls_prompt_gameplay_body(0);
 assert(recomp_controls_prompt_binding(8,XBOX_PROMPT_KEYBOARD,-1)==VK_SPACE);
 puts("PASS: all scope actions/sources, defaults, shared aliases, migration, persistence and tutorial prompts");
}
"""
with tempfile.TemporaryDirectory(prefix='merc-scope-') as folder:
 d=Path(folder);(d/'test.c').write_text(source);exe=d/'test.exe'
 subprocess.run(['C:/MinGW/bin/gcc.exe','-std=c11','-D_WIN32_WINNT=0x0601','-O2','-I'+str(ROOT/'ports/mercenaries/src'),'-I'+str(ROOT/'src/input'),'-I'+str(ROOT/'src'),str(d/'test.c'),'-o',str(exe),'-luser32'],check=True)
 subprocess.run([str(exe),str(d)],check=True)
