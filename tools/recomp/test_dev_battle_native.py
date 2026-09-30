"""Exercise developer targeting policy against bounded guest-memory fixtures."""
from pathlib import Path
import subprocess,tempfile
ROOT=Path(__file__).resolve().parents[2]
source=r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "dev_menu.h"
static unsigned char ram[0x4000000];
static uint32_t g_xbox_mem_offset=0x10000,g_recomp_live_player_ai=0x100000;
static unsigned flags;
unsigned recomp_dev_battle_flags(void){return flags;}
static int dev_guest_address(uint32_t p,uint32_t n){return p>=0x10000 && n<sizeof(ram) && p<=sizeof(ram)-n;}
static uint32_t guest_u32(uint32_t p){assert(dev_guest_address(p,4));uint32_t v;memcpy(&v,ram+p,4);return v;}
static void put(uint32_t p,uint32_t v){memcpy(ram+p,&v,4);}
static uint32_t g_esp=0x800000;
typedef struct {uint32_t esp;} recomp_saved_guest_cpu_context;
static void recomp_save_guest_cpu_context(recomp_saved_guest_cpu_context *s){s->esp=g_esp;}
static void recomp_restore_guest_cpu_context(const recomp_saved_guest_cpu_context *s){g_esp=s->esp;}
static int recomp_lookup(uint32_t va){return 1;}
static uint32_t dev_call(uint32_t stack,uint32_t va,uint32_t obj,unsigned n,const uint32_t *args){return 0x110000;}
#include "dev_battle_guest.h"
int main(void){
 const uint32_t ai=0x200000,player=0x110000,enemy=0x210000,seat=0x120000,manager=0x130000,vehicle=0x140000;
 put(0x413F6C,0x4249D707);put(player+4,0x660E4490);put(g_recomp_live_player_ai+0x10,player);
 flags=0;assert(!recomp_dev_battle_protected(0xffffffff,0xffffffff));
 flags=1;recomp_dev_battle_refresh_player();assert(recomp_dev_battle_protected(ai,player));assert(!recomp_dev_battle_protected(ai,enemy));assert(!recomp_dev_battle_protected(ai,vehicle));
 put(player+0x768,seat);put(seat,manager);put(manager,vehicle);
 assert(recomp_dev_battle_protected(ai,vehicle));assert(!recomp_dev_battle_protected(g_recomp_live_player_ai,enemy));
 put(player+0x200,g_recomp_live_player_ai);put(g_recomp_live_player_ai+0x10,vehicle);
 flags=2;assert(recomp_dev_battle_protected(ai,enemy));assert(recomp_dev_battle_protected(ai,player));assert(!recomp_dev_battle_protected(g_recomp_live_player_ai,enemy));
 flags=3;assert(recomp_dev_battle_protected(ai,vehicle));
 flags=0;assert(!recomp_dev_battle_protected(ai,vehicle));assert(!recomp_dev_battle_protected(ai,player));
 flags=1;put(player+0x768,0);assert(!recomp_dev_battle_protected(ai,vehicle));
 put(player+4,0);assert(!recomp_dev_battle_protected(ai,player));
 flags=2;put(0x413F6C,0);assert(!recomp_dev_battle_protected(ai,enemy));
 assert(guest_u32(seat)==manager && guest_u32(manager)==vehicle);
 puts("PASS: player, occupied vehicle, other factions, passive/player distinction, immediate disable, stale pointers and transition gates");
}
'''
with tempfile.TemporaryDirectory(prefix='merc-battle-') as d:
 p=Path(d);(p/'test.c').write_text(source)
 subprocess.run(['C:/MinGW/bin/gcc.exe','-std=c11','-O2','-I',str(ROOT/'ports/mercenaries/src'),str(p/'test.c'),'-o',str(p/'test.exe')],check=True)
 subprocess.run([str(p/'test.exe')],check=True)
