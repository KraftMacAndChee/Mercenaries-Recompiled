"""Exercise rendering selection against production controls and supplied artwork."""
from pathlib import Path
import ast,subprocess,tempfile
ROOT=Path(__file__).resolve().parents[2]
module=ast.parse((ROOT/'tools/recomp/test_controls_native.py').read_text())
source=next(ast.literal_eval(n.value) for n in module.body if isinstance(n,ast.Assign) and any(isinstance(t,ast.Name) and t.id=='source' for t in n.targets))
source=source[:source.index('int main(')]
source+=r"""
#include <stddef.h>
static int test_device,test_fixed;
int recomp_options_fixed_xbox_prompts(void){return test_fixed;}
static float test_prompt_aspect=1.f;
float recomp_ui_prompt_aspect(void){return test_prompt_aspect;}
int xbox_InputPromptDevice(void){return test_device;}
static int test_controller;
int xbox_InputPromptController(void){return test_controller;}
ptrdiff_t g_xbox_mem_offset;
static unsigned allocation=0x100000,uploads;
uint32_t recomp_title_heap_allocate(uint32_t size){unsigned p=allocation;allocation+=size;return p;}
void recomp_title_heap_free(uint32_t p){(void)p;}
int pgraph_d3d11_register_ui_texture(uint32_t offset,uint32_t w,uint32_t h,const uint32_t *pixels){
 assert(offset>=0x100000 && !(offset&127));assert(w==64&&h==64&&pixels);++uploads;return 1;
}
#include "recomp_prompts.c"
int main(int argc,char **argv){
 assert(argc==2);snprintf(test_ini,sizeof(test_ini),"%s\\runtime.exe",argv[1]);
 recomp_controls_init((HWND)1);play(4);
 unsigned char *memory=calloc(1,4*1024*1024);assert(memory);g_xbox_mem_offset=(ptrdiff_t)memory;
 unsigned original=0x10000;write32(original+0x44,0x11000);write32(0x11004,0x20000);
 unsigned hash=0,object;
 test_device=XBOX_PROMPT_KEYBOARD;
 object=recomp_prompts_region(0xFB0968D1u,original,&hash);
 assert(object && hash==PROMPT_HASH_BASE+PROMPT_KEY_1366); /* V */
 assert(recomp_prompts_find_texture(hash,0)==object);
 assert(recomp_prompts_find_texture(hash,original)==original); /* no real resource collision */
 unsigned before=uploads;assert(recomp_prompts_region(0xFB0968D1u,original,&hash)==object);assert(uploads==before);
 controls_assign(g_binds[0][3],11,'E');
 assert(recomp_prompts_region(0xFB0968D1u,original,&hash)!=object);assert(hash==PROMPT_HASH_BASE+PROMPT_KEY_1345);
 test_device=XBOX_PROMPT_PLAYSTATION;recomp_prompts_region(0xFB0968D1u,original,&hash);assert(hash==PROMPT_HASH_BASE+PROMPT_PS_Y);
 controls_assign(g_binds[1][3],11,10);recomp_prompts_region(0xFB0968D1u,original,&hash);assert(hash==PROMPT_HASH_BASE+PROMPT_PS_X);
 test_device=XBOX_PROMPT_XBOX;recomp_prompts_region(0xFB0968D1u,original,&hash);assert(hash==PROMPT_HASH_BASE+PROMPT_XBOX_X);
 test_device=XBOX_PROMPT_KEYBOARD;recomp_prompts_region(0xFB0968D1u,original,&hash);assert(hash==PROMPT_HASH_BASE+PROMPT_KEY_1345);
 controls_assign(g_binds[0][3],11,VK_F24);recomp_prompts_region(0xFB0968D1u,original,&hash);assert(hash==PROMPT_HASH_BASE+PROMPT_KEY_1411);
 assert(keyboard_asset(VK_F12)==PROMPT_KEY_1329);assert(keyboard_asset(VK_XBUTTON2)==PROMPT_KEY_1600);
 controls_assign(g_binds[0][11],14,'R');recomp_prompts_region(0x5E1743CAu,original,&hash);
 unsigned pda=hash;assert(hash>=PROMPT_HASH_BASE+PROMPT_ASSET_COUNT);
 assert(textures[hash-PROMPT_HASH_BASE].pixels[32*64]==0); /* transparent horizontal padding */
 controls_assign(g_binds[0][11],14,'T');recomp_prompts_region(0x5E1743CAu,original,&hash);assert(hash!=pda);
 play(13);recomp_prompts_region(0xFB0968D1u,original,&hash);assert(hash==PROMPT_HASH_BASE+PROMPT_KEY_1411);
 g_pause_navigation=1;recomp_prompts_region(0xCCDC1675u,original,&hash);assert(hash==PROMPT_HASH_BASE+PROMPT_KEY_1389);
 g_pause_navigation=0;play(4);
 recomp_prompts_movement_scope(1);recomp_prompts_region(0x89EEF07Bu,original,&hash);assert(hash>=PROMPT_HASH_BASE+PROMPT_ASSET_COUNT);
 unsigned group=hash;controls_assign(g_binds[0][3],19,'T');recomp_prompts_region(0x89EEF07Bu,original,&hash);assert(hash!=group);
 recomp_prompts_movement_scope(0);recomp_prompts_region(0x89EEF07Bu,original,&hash);assert(hash==PROMPT_HASH_BASE+PROMPT_KEY_1368); /* L3 remains N */
 assert(!recomp_prompts_region(0x12345678,original,&hash));
 test_fixed=1;before=uploads;
 for(int family=0;family<3;++family){
  test_device=family;recomp_prompts_movement_scope(1);recomp_prompts_support_scope(1);
  for(unsigned r=0;r<sizeof(regions)/sizeof(regions[0]);++r){
   hash=0xdeadbeef;assert(!recomp_prompts_region(regions[r].region,original,&hash));assert(hash==0xdeadbeef);
  }
  assert(!recomp_prompts_region(0x64471311,original,&hash));
  assert(!recomp_prompts_region(0x55034E83,original,&hash));
 }
 assert(uploads==before);test_fixed=0;recomp_prompts_movement_scope(0);recomp_prompts_support_scope(0);

 assert(keyboard_asset(0x100)==PROMPT_KEY_1613 && keyboard_asset(0x101)==PROMPT_KEY_1624);
 for(unsigned i=0;i<PROMPT_ASSET_COUNT;++i){uint32_t *pixels=decode_asset(i);assert(pixels);free(pixels);}
 test_prompt_aspect=4.f/3.f;test_device=XBOX_PROMPT_KEYBOARD;
 controls_assign(g_binds[0][3],11,'E');recomp_prompts_region(0xFB0968D1u,original,&hash);
 assert(hash>=PROMPT_HASH_BASE+PROMPT_ASSET_COUNT);
 assert(textures[hash-PROMPT_HASH_BASE].pixels[32*64]==0);
 assert(read32(original+0x44)==0x11000&&read32(0x11004)==0x20000); /* original atlas untouched */

 /* Editor device is independent of last-active gameplay device. */
 g_menu=2;g_page=0;g_edit_context=3;g_device=0;g_listen=-1;test_prompt_aspect=1.f;
 unsigned rowhash=RECOMP_CONTROLS_ROW+2;int keyboard;unsigned short assigned;
 controls_assign(g_binds[0][3],19,'E');
 assert(recomp_controls_row_binding(rowhash,&keyboard,&assigned)&&keyboard&&assigned=='E');
 assert(!strcmp(recomp_controls_label(rowhash),"WALK FORWARD"));
 recomp_prompts_binding_row(rowhash);test_device=XBOX_PROMPT_PLAYSTATION;
 recomp_prompts_region(0xCCDC1675u,original,&hash);assert(hash==PROMPT_HASH_BASE+PROMPT_KEY_1345);
 g_device=1;test_device=XBOX_PROMPT_KEYBOARD;test_controller=XBOX_PROMPT_PLAYSTATION;
 controls_assign(g_binds[1][3],19,11);
 recomp_prompts_region(0xCCDC1675u,original,&hash);assert(hash==PROMPT_HASH_BASE+PROMPT_PS_Y);
 controls_assign(g_binds[1][3],19,10);
 recomp_prompts_region(0xCCDC1675u,original,&hash);assert(hash==PROMPT_HASH_BASE+PROMPT_PS_X);
 test_controller=XBOX_PROMPT_XBOX;
 recomp_prompts_region(0xCCDC1675u,original,&hash);assert(hash==PROMPT_HASH_BASE+PROMPT_XBOX_X);
 test_fixed=1;g_device=0;test_device=XBOX_PROMPT_XBOX;
 recomp_prompts_region(0xCCDC1675u,original,&hash);assert(hash==PROMPT_HASH_BASE+PROMPT_KEY_1345);
 g_page=1;controls_assign(g_binds[0][3],23,VK_F24);
 recomp_prompts_region(0xCCDC1675u,original,&hash);assert(hash==PROMPT_HASH_BASE+PROMPT_KEY_1411);
 g_edit_context=0;controls_assign(g_binds[0][0],23,'R');
 recomp_prompts_region(0xCCDC1675u,original,&hash);assert(hash==PROMPT_HASH_BASE+PROMPT_KEY_1346);
 g_listen=23;assert(!recomp_controls_row_binding(rowhash,0,0));assert(strstr(recomp_controls_label(rowhash),"PRESS INPUT"));
 g_listen=-1;g_binds[0][0][23]=0xffff;assert(!recomp_controls_row_binding(rowhash,0,0));assert(strstr(recomp_controls_label(rowhash),"UNBOUND"));
 recomp_prompts_binding_row(0);g_menu=0;g_pause_navigation=1;
 /* Fixed Xbox retains original geometry. Dynamic pulse follows fitted alpha. */
 unsigned args=0x30000;float authored[4]={10,20,28,28},result[4];
 memcpy(guest(args),authored,sizeof(authored));recomp_prompts_menu_glow(args);assert(!memcmp(guest(args),authored,sizeof(authored)));
 test_fixed=0;
 for(int family=0;family<3;++family)for(int aspect=0;aspect<2;++aspect){
  test_device=family;test_prompt_aspect=aspect?4.f/3.f:1.f;
  memcpy(guest(args),authored,sizeof(authored));recomp_prompts_menu_glow(args);memcpy(result,guest(args),sizeof(result));
  unsigned bind=recomp_controls_prompt_binding(8,family,-1);
  unsigned asset=fit_region_asset(family==2?keyboard_asset(bind):pad_asset(bind,family),0xCCDC1675u);
  uint32_t *temp=NULL;const uint32_t *pixels=textures[asset].pixels;if(!pixels)pixels=temp=decode_asset(asset);
  unsigned peak=0,x0=64,y0=64,x1=0,y1=0;
  for(unsigned i=0;i<4096;++i)if((pixels[i]>>24)>peak)peak=pixels[i]>>24;
  for(unsigned y=0;y<64;++y)for(unsigned x=0;x<64;++x)if((pixels[y*64+x]>>24)>=(peak+1)/2){if(x<x0)x0=x;if(y<y0)y0=y;if(x+1>x1)x1=x+1;if(y+1>y1)y1=y+1;}
  /* Original glow/icon = 28/20; fitted icon occupies extent/64 of 20. */
  assert(fabsf(result[2]/(20.f*(x1-x0)/64.f)-1.4f)<.0001f);
  assert(fabsf(result[3]/(20.f*(y1-y0)/64.f)-1.4f)<.0001f);
  assert(fabsf(result[0]+result[2]*.5f-(24.f+((x0+x1)*.5f-32.f)*20.f/64.f))<.0001f);
  free(temp);
 }

 /* Modal tutorial body uses gameplay maps for helicopters, cars, tanks,
  * on-foot and scope. Its Continue footer uses Menus. Exercise actual
  * glyph selection after rebinding, not merely the resolver's result. */
 test_prompt_aspect=1.f;test_fixed=0;g_pause_navigation=0;
 for(unsigned c=0;c<5;++c)for(int family=0;family<3;++family){
  reset_input();play(c+1);recomp_controls_substate(0);
  recomp_controls_state(0x4249D707,0xDDFB69D8,15,0);recomp_controls_substate(5);
  test_device=family;int d=family==XBOX_PROMPT_KEYBOARD?0:1;
  recomp_controls_prompt_gameplay_body(1);
  for(unsigned r=0;r<sizeof(regions)/sizeof(regions[0]);++r){
   unsigned action=regions[r].action;
   if(regions[r].context>=0 || action>=24 || !action_available(c,action))continue;
   unsigned short old=g_binds[d][controls_binding_context(c)][action];
   g_binds[d][controls_binding_context(c)][action]=d?10:'T';
   object=recomp_prompts_region(regions[r].region,original,&hash);
   unsigned expected=d?pad_asset(10,family):keyboard_asset('T');
   assert(object && hash==PROMPT_HASH_BASE+fit_region_asset(expected,regions[r].region));
   g_binds[d][controls_binding_context(c)][action]=old;
  }
  /* [move]/[aim], used by both the helicopter and tank tutorials. */
  if(c==0 || c==2){
   recomp_prompts_movement_scope(1);
   recomp_prompts_region(0x89EEF07Bu,original,&hash);assert(hash!=PROMPT_HASH_BASE+PROMPT_KEY_1411);
   recomp_prompts_region(0x77EED425u,original,&hash);assert(hash!=PROMPT_HASH_BASE+PROMPT_KEY_1411);
   recomp_prompts_movement_scope(0);
  }
  if(c==0 && family==XBOX_PROMPT_KEYBOARD){
   unsigned asset=group_asset(0,family);assert(asset>=PROMPT_ASSET_COUNT);
   /* D-pad Left is the restored vehicle Toggle Flags input. */
   unsigned left_visible=0;
   for(unsigned y=32;y<53;++y)for(unsigned x=0;x<21;++x)left_visible|=textures[asset].pixels[y*64+x];
   assert(left_visible);
   unsigned short old=g_binds[0][0][0];g_binds[0][0][0]=0xffffu;
   asset=group_asset(0,family);unsigned visible=0;
   for(unsigned y=10;y<31;++y)for(unsigned x=21;x<42;++x)visible|=textures[asset].pixels[y*64+x];
   assert(visible);g_binds[0][0][0]=old;
  }
  recomp_controls_prompt_gameplay_body(0);
  recomp_prompts_region(0x4D813BE7u,original,&hash);
  unsigned b=resolved_binding(d,MENU_CONTEXT,8);
  assert(hash==PROMPT_HASH_BASE+fit_region_asset(d?pad_asset(b,family):keyboard_asset(b),0x4D813BE7u));
 }

 /* The support carousel advertises Menu Select Support in all phases.
  * Menu Confirm remains a separate binding. Test distinct custom rebinds. */
 for(int family=0;family<3;++family){
  test_device=family;int d=family==XBOX_PROMPT_KEYBOARD?0:1;
  reset_input();play(4);recomp_controls_substate(0);
  g_binds[d][MENU_CONTEXT][8]=d?10:'T';
  g_binds[d][3][15]=d?14:'V';
  g_binds[d][MENU_CONTEXT][15]=d?15:'P';
  for(unsigned sub=0;sub<=2;++sub){
   recomp_controls_substate(sub);
   recomp_prompts_support_fire_scope(1);
   recomp_prompts_region(0xF880B618u,original,&hash);
   assert(hash==PROMPT_HASH_BASE+fit_region_asset(d?pad_asset(15,family):keyboard_asset('P'),0xF880B618u));
   test_fixed=1;assert(!recomp_prompts_region(0xF880B618u,original,&hash));test_fixed=0;
   recomp_prompts_support_fire_scope(0);
  }
  recomp_controls_substate(0);
  recomp_prompts_region(0xF880B618u,original,&hash);
  assert(hash==PROMPT_HASH_BASE+fit_region_asset(d?pad_asset(14,family):keyboard_asset('V'),0xF880B618u));
 }
 puts("support selection stays stable through menu/fade; equipped Fire is independent; Fixed Xbox preserves retail");
 puts("tutorials: vehicle/foot bodies, custom binds, three device families, move/aim groups and menu footer pass");
 puts("editor icons: selected device, retained pad family, page/context, rebind, unknown/capture/unbound; glow ratios pass");
 puts("prompts: live device/rebinding/context selection, cache, fallback and all artwork pass");return 0;
}
"""
with tempfile.TemporaryDirectory(prefix='merc-prompts-') as folder:
 d=Path(folder);(d/'test.c').write_text(source,encoding='utf8');exe=d/'test.exe'
 cmd=['C:/MinGW/bin/gcc.exe','-std=c11','-D_WIN32_WINNT=0x0601','-O1']
 cmd += ['-I'+str(ROOT/p) for p in ['ports/mercenaries/src','src/input','src/nv2a','src','include']]
 subprocess.run(cmd+[str(d/'test.c'),'-o',str(exe),'-luser32','-lm'],check=True)
 subprocess.run([str(exe),str(d)],check=True)
