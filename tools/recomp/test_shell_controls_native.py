"""Exercise real shell/pause menu insertion and restoration in bounded guest memory."""
from pathlib import Path
import subprocess, tempfile
ROOT=Path(__file__).resolve().parents[2]
production=(ROOT/'ports/mercenaries/src/recomp_options_menu.c').read_text().replace('#include "recomp/recomp_types.h"','')
prefix=r'''
#include <stdint.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <float.h>
#include <math.h>
static unsigned char ram[0x800000];
#define XBOX_PTR(a) (ram+(a))
#define MEM32(a) (*(uint32_t*)XBOX_PTR(a))
#define MEMF(a) (*(float*)XBOX_PTR(a))
static uint32_t heap=0x20000;static int opened,freed;
uint32_t recomp_title_heap_allocate(uint32_t n){uint32_t p=heap;heap+=n;assert(heap<sizeof(ram));return p;}
void recomp_title_heap_free(uint32_t p){assert(p>=0x20000 && p<heap);++freed;}
void recomp_options_cancel_edit(void){} void recomp_options_begin_edit(void){}
uint32_t recomp_options_apply(void){return 0;}int recomp_options_adjust(uint32_t h,int d){return 0;}
const char*recomp_options_label(uint32_t h){return 0;}
void recomp_controls_open(void){opened=1;}void recomp_controls_close(void){opened=0;}
static unsigned rows=5;
unsigned recomp_controls_count(void){return rows;}
uint32_t recomp_controls_input(unsigned r,unsigned i,unsigned e){if(i==5 && e==1 && r==3)rows=9;else if(i==4 && e==1)rows=5;return 1;}
const char*recomp_controls_label(uint32_t h){return 0;}
'''
suffix=r'''
int main(void){
 uint32_t owner=0x10000,menu=0x15000,next=0x16000;
 w32(menu+0x54,MENU_MAIN_HASH);w32(menu+0x44,3);
 w32(menu+4,allocate_item(111,0));w32(menu+8,allocate_item(MENU_SHELL_OPTIONS_HASH,14));w32(menu+12,allocate_item(222,0));
 insert_entry(menu);assert(u32(menu+0x44)==7);
 assert(item_hash(menu,0)==111 && item_hash(menu,1)==0x8EAD97C8 && item_hash(menu,2)==RECOMP_OPTIONS_MENU_HASH);
 assert(item_hash(menu,3)==MENU_SHELL_OPTIONS_HASH && item_hash(menu,4)==222);
 assert(item_hash(menu,5)==RECOMP_ACK_MENU_HASH && item_hash(menu,6)==RECOMP_QUIT_HASH);
 uint32_t after=heap;for(int i=0;i<100;++i)insert_entry(menu);assert(heap==after && u32(menu+0x44)==7);
 w32(owner+0x3EB8,menu);w32(menu+0x48,1);w32(next+0x54,MENU_SHELL_OPTIONS_HASH);w32(next+0x44,1);
 uint32_t original=allocate_item(333,0);w32(next+4,original);
 recomp_options_menu_transition(owner,next);assert(opened && g_menu.active && g_menu.controls && u32(next+0x44)==5);

 /* Retail opening only schedules count+1 rows. Expanding Controls in-place
    used to leave rows 6..8 at FLT_MAX, so their computed opacity stayed zero. */
 uint32_t brush=0x17000;float per=.1f,timer=2.f;
 w32(brush+0x34,3);w32(brush+0x3C,5);
 memcpy(ram+brush+0xC8,&per,4);memcpy(ram+brush+0xC4,&timer,4);
 for(unsigned j=0;j<17;++j){float f=j<=5?j*per:FLT_MAX;memcpy(ram+brush+0x80+4*j,&f,4);}
 w32(next+0x48,3);recomp_options_input_checkpoint(next,5,1);
 assert(u32(next+0x44)==9 && u32(next+0x48)==0);
 w32(brush+0x3C,9);
 float old;memcpy(&old,ram+brush+0x98,4);assert((timer-old)/per<0);
 recomp_options_prepare_menu_paint(next,brush);
 for(unsigned j=0;j<=9;++j){float f;memcpy(&f,ram+brush+0x80+4*j,4);assert(fabsf(f-j*per)<.000001f);assert((timer-f)/per>1.f);}
 assert(!memcmp(ram+brush+0xC4,&timer,4));
 /* Paging/repeated visits retain visibility; opening/closing and unrelated
    menus must not have their fades rewritten. */
 unsigned char snapshot[0xE0];memcpy(snapshot,ram+brush,sizeof(snapshot));
 recomp_options_prepare_menu_paint(menu,brush);assert(!memcmp(snapshot,ram+brush,sizeof(snapshot)));
 for(unsigned state=0;state<3;++state){w32(brush+0x34,state);w32(brush+0x98,0x7f7fffff);recomp_options_prepare_menu_paint(next,brush);assert(u32(brush+0x98)==0x7f7fffff);}
 w32(brush+0x34,3);
 for(unsigned visit=0;visit<10;++visit){
   recomp_options_input_checkpoint(next,4,1);assert(u32(next+0x44)==5);
   w32(brush+0x3C,5);recomp_options_prepare_menu_paint(next,brush);
   w32(next+0x48,3);recomp_options_input_checkpoint(next,5,1);
   w32(brush+0x3C,9);recomp_options_prepare_menu_paint(next,brush);
   float f;memcpy(&f,ram+brush+0xA0,4);assert(f==8*per);
 }
 w32(owner+0x3EB8,next);recomp_options_menu_transition(owner,menu);
 assert(!opened && !g_menu.active && u32(next+0x44)==1 && u32(next+4)==original && freed==RECOMP_CONTROLS_ROWS);
 w32(menu+0x54,MENU_PAUSE_HASH);insert_entry(menu);assert(u32(menu+0x44)==7);
 /* Full menus cannot overflow their sixteen-item storage. */
 memset(ram+menu,0,0x80);w32(menu+0x54,MENU_MAIN_HASH);w32(menu+0x44,16);
 for(int i=0;i<16;++i)w32(menu+4+i*4,allocate_item(1000+i,0));
 after=heap;insert_entry(menu);assert(heap==after && u32(menu+0x44)==16);
 puts("shell controls: insertion, repeated visits, open/back, pause and capacity passed");
}
'''
with tempfile.TemporaryDirectory(prefix='merc-shell-controls-') as td:
 p=Path(td);(p/'test.c').write_text(prefix+production+suffix)
 subprocess.run(['C:/MinGW/bin/gcc.exe','-std=c11','-O2','-I'+str(ROOT/'ports/mercenaries/src'),'-I'+str(ROOT/'src/input'),'-I'+str(ROOT/'src'),'-I'+str(ROOT/'include'),str(p/'test.c'),'-o',str(p/'test.exe')],check=True)
 subprocess.run([str(p/'test.exe')],check=True)
