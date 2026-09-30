"""Check QWERTY defaults and non-destructive upgrade/reset behavior in production code."""
import ast
from pathlib import Path
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[2]
module = ast.parse((ROOT / 'tools/recomp/test_controls_native.py').read_text(encoding='utf-8'))
source = next(ast.literal_eval(n.value) for n in module.body if isinstance(n, ast.Assign) and any(isinstance(t, ast.Name) and t.id == 'source' for t in n.targets))
# Reuse OS/device mocks only; this suite independently tests the new profile.
source = source[:source.index('static void test_free_camera(void)')]
source = source.replace('/*DEVELOPER_HOTKEY*/', '')
source += r"""
static void reload(void){reset_input();g_initialized=0;recomp_controls_init((HWND)1);}
static void key_output(unsigned joy,unsigned key,unsigned action){
 reset_input();g_wheel_pending=0;g_wheel_release=0;g_wheel_active=0;
 if(key==WHEEL_UP || key==WHEEL_DOWN)g_wheel_pending=key==WHEEL_UP?1:-1;
 else test_keys[key]=1;
 play(joy);
 XBOX_INPUT_STATE state={0};assert(mapper(&state,TRUE));float out[24];unpack(&state.Gamepad,out);
 if(out[action]<=.99f)fprintf(stderr,"joy=%u key=%u action=%u value=%f\n",joy,key,action,out[action]);
 assert(out[action]>.99f);
}
int main(int argc,char**argv){
 assert(argc==2);g_xbox_mem_offset=(ptrdiff_t)camera_memory-0x10000;
 snprintf(test_ini,sizeof(test_ini),"%s\\runtime.exe",argv[1]);reload();
 assert(GetPrivateProfileIntA("Controls","KeyboardDefaultsVersion",0,g_path)==2);
 assert(g_mouse==1);
 const unsigned short expected[24]={VK_XBUTTON2,VK_XBUTTON1,VK_LEFT,VK_RIGHT,VK_ESCAPE,VK_TAB,VK_LCONTROL,'Q','R',VK_SPACE,VK_MBUTTON,'E',WHEEL_UP,WHEEL_DOWN,VK_RBUTTON,VK_LBUTTON,'A','D','S','W','J','L','K','I'};
 for(unsigned a=0;a<24;++a)assert(g_binds[0][3][a]==expected[a]);
 /* Explicit QWERTY even if the OS reports an AZERTY layout. */
 test_azerty=1;assert(default_binding(0,3,19)=='W' && default_binding(0,3,16)=='A' && default_binding(0,3,7)=='Q');test_azerty=0;
 for(int c=0;c<CONTROL_CONTEXTS;++c)for(unsigned a=0;a<26;++a)
  assert(g_binds[1][c][a]==legacy_default_binding(1,c,a));
 for(unsigned a=0;a<20;++a)key_output(4,expected[a],a);
 key_output(5,WHEEL_UP,19);key_output(10,WHEEL_DOWN,18);key_output(10,VK_LBUTTON,15);
 /* Vehicle inputs must reach the retail channels actually used by each mode. */
 key_output(1,'F',0);key_output(1,'V',1);key_output(1,VK_SPACE,8);key_output(1,'R',10);
 key_output(2,'W',8);key_output(2,'S',10);key_output(2,VK_SPACE,15);key_output(2,VK_RBUTTON,12);
 key_output(3,'W',19);key_output(3,'S',18);key_output(3,VK_LBUTTON,15);
 for(unsigned joy=1;joy<=3;++joy){
  key_output(joy,'A',16);key_output(joy,'D',17);key_output(joy,'E',11);
  key_output(joy,VK_MBUTTON,9);key_output(joy,VK_XBUTTON1,13);key_output(joy,VK_XBUTTON2,14);
  if(joy!=2)key_output(joy,WHEEL_UP,12);
 }
 key_output(12,'Q',14);key_output(12,'E',15);key_output(12,VK_SPACE,8);
 key_output(12,'R',11);key_output(12,VK_TAB,5);
 key_output(12,'W',19);key_output(12,'A',16);key_output(12,'S',18);key_output(12,'D',17);
 for(unsigned i=0;i<6;++i){
  const unsigned mk[]={'W','S','A','D',VK_SPACE,'R'}, ma[]={0,1,2,3,8,9};
  reset_input();recomp_controls_state(0x59C00449,0xC2CBD863,4,0);test_keys[mk[i]]=1;
  XBOX_INPUT_STATE state={0};assert(mapper(&state,TRUE));float out[24];unpack(&state.Gamepad,out);assert(out[ma[i]]>.99f);
 }
 assert(g_binds[0][MENU_CONTEXT][11]=='R');
 /* Saved new profiles, including explicit unbinding, are never re-defaulted. */
 g_device=0;g_edit_context=3;g_binds[0][3][15]='T';g_binds[0][3][6]=0xffff;save_context();
 write_value("Controls","MouseAim",0);reload();assert(!g_mouse);
 assert(g_binds[0][3][15]=='T' && g_binds[0][3][6]==0xffff);
 /* A pre-upgrade, partially saved profile keeps old implicit defaults and all
  * explicit keyboard/gamepad settings, including deliberate menu UNBOUND. */
 assert(DeleteFileA(g_path));
 write_value("Keyboard.4","Action15",'P');write_value("Keyboard.4","Action6",0xffff);
 write_value("Keyboard.13","Action8",VK_LBUTTON);write_value("Keyboard.13","Action11",0xffff);
 write_value("Gamepad.4","Action15",8);write_value("Controls","MenuBindingsVersion",4);
 write_value("Controls","ScopeBindingsVersion",1);reload();
 assert(GetPrivateProfileIntA("Controls","KeyboardDefaultsVersion",0,g_path)==1);
 assert(!g_mouse && g_binds[0][3][8]=='Z' && g_binds[0][3][11]=='V');
 assert(g_binds[0][3][15]=='P' && g_binds[0][3][6]==0xffff && g_binds[1][3][15]==8);
 assert(g_binds[0][MENU_CONTEXT][8]==VK_LBUTTON && g_binds[0][MENU_CONTEXT][11]==0xffff);
 assert(GetPrivateProfileIntA("Keyboard.4","Action8",-1,g_path)==(UINT)-1);
 unsigned short old[2][CONTROL_CONTEXTS][CONTROL_ACTIONS];memcpy(old,g_binds,sizeof(old));
 reload();assert(!memcmp(old,g_binds,sizeof(old)));
 /* Explicit Reset adopts QWERTY in one context; every other binding survives
  * immediately and on restart, including untouched old fallback contexts. */
 recomp_controls_open();g_menu=2;g_device=0;g_edit_context=3;g_page=0;
 recomp_controls_input(3+visible_actions(),5,1);
 for(unsigned a=0;a<26;++a)assert(g_binds[0][3][a]==default_binding(0,3,a));
 for(int d=0;d<2;++d)for(int c=0;c<CONTROL_CONTEXTS;++c)if(d || c!=3)
  assert(!memcmp(old[d][c],g_binds[d][c],sizeof(old[d][c])));
 recomp_controls_close();memcpy(old,g_binds,sizeof(old));reload();assert(!memcmp(old,g_binds,sizeof(old)));
 assert(!g_mouse); /* Reset binds does not change a saved mouse preference. */
 puts("keyboard defaults: QWERTY routing, scope/vehicles/PDA/menus, persistence and legacy preservation passed");
 return 0;
}
"""
with tempfile.TemporaryDirectory(prefix='merc-defaults-') as folder:
    d=Path(folder)
    (d/'test.c').write_text(source,encoding='utf-8')
    exe=d/'test.exe'
    command=['C:/MinGW/bin/gcc.exe','-std=c11','-D_WIN32_WINNT=0x0601','-O2',*[ '-I'+str(ROOT/p) for p in ['ports/mercenaries/src','src/input','src','include']],str(d/'test.c'),'-o',str(exe),'-luser32','-lm']
    subprocess.run(command,check=True)
    subprocess.run([str(exe),str(d)],check=True)
