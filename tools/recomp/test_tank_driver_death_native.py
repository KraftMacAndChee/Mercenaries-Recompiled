"""Preserve retail latched controls when a vehicle driver dies."""
from pathlib import Path
import re, subprocess, tempfile
ROOT=Path(__file__).resolve().parents[2]

def test_dead_tank_driver_preserves_retail_controls():
    gen=ROOT/'ports/mercenaries/src/recomp/gen'
    def body(file,name):
        return re.search(r'void '+name+r'\(void\)\n\{.*?\n\}',(gen/file).read_text(),re.S)[0]
    death=body('recomp_0006.c','sub_00166AB0')
    assert 'dead_driver_vehicle' not in death
    assert 'sub_0005DD20' not in death
    pre=r"""
#define RECOMP_GENERATED_CODE
#include "recomp_types.h"
#include <assert.h>
#include <stdio.h>
uint32_t g_eax,g_ecx,g_edx,g_esp,g_ebx,g_esi,g_edi,g_seh_ebp,g_fp_top;
ptrdiff_t g_xbox_mem_offset;double g_fp_stack[8];
float g_xmm0[4],g_xmm1[4],g_xmm2[4],g_xmm3[4],g_xmm4[4],g_xmm5[4],g_xmm6[4],g_xmm7[4];
static unsigned char memory[0x10000];
static unsigned player,anim,kicks,disconnects,eliminates;
enum {STATE=0x1000,SEAT=0x2000,MANAGER=0x3000,RIDER=0x4000,VEHICLE=0x5000,VT=0x6000,RVT=0x7000,STACK=0xe000};
#undef RECOMP_TRACE_FUNC
#define RECOMP_TRACE_FUNC(a) ((void)0)
#undef RECOMP_ICALL_SAFE
#define RECOMP_ICALL_SAFE(a,s) callback(a)
static void callback(uint32_t a){
 switch(a){
 case 1: assert(ecx==RIDER);eax=player;esp+=4;break;
 case 2: assert(ecx==VEHICLE);assert(!MEM32(esp+4));++kicks;esp+=8;
         eax=0xbad;ecx=0xbad;edx=0xbad;break; /* realistic caller-clobbered regs */
 case 3: assert(ecx==RIDER);++eliminates;esp+=4;break;
 default:assert(0);
 }
}
static void sub_001624B0(void){assert(ecx==SEAT);eax=RIDER;esp+=4;}
static void sub_00164770(void){assert(ecx==SEAT);esp+=12;}
static void sub_0004CB90(void){assert(ecx==RIDER);esp+=8;}
static void sub_00061E50(void){eax=anim;esp+=16;}
static void sub_001662D0(void){assert(ecx==SEAT);++disconnects;esp+=4;}
"""
    main=r"""
int main(void){
 g_xbox_mem_offset=(ptrdiff_t)memory;unsigned cases=0;
 for(player=0;player<2;++player)for(unsigned seat=0;seat<3;++seat)
 for(unsigned vehicle=0;vehicle<3;++vehicle)for(anim=0;anim<2;++anim){
  memset(memory,0,sizeof(memory));kicks=disconnects=eliminates=0;
  MEM32(STATE+4)=SEAT;MEM32(SEAT)=MANAGER;MEM32(SEAT+0x114)=seat;
  MEM32(MANAGER)=vehicle?VEHICLE:0;MEM8(MANAGER+0x8ca)=4;
  MEM32(RIDER)=RVT;MEM32(RVT+0xf0)=1;MEM32(RVT+0x10)=3;MEM32(RIDER+0x6b0)=0x8000;
  MEM32(VEHICLE)=VT;MEM32(VT+0x1c0)=2;MEM32(VT+0x1e0)=vehicle==1?0x5dd20:0x12345;
  unsigned physics=VEHICLE+0x2e4;
  /* Four real controls: throttle, brake, steering, forward tread speed. */
  for(unsigned i=0;i<4;++i)MEMF(physics+0x94+4*i)=(float)(i+1)*.25f;
  MEM8(physics+0xa4)=1;
  unsigned char before[0x200];memcpy(before,memory+physics,sizeof(before));
  ecx=STATE;esp=STACK;ebx=0x9876;esi=0x6543;edi=0x1234;g_seh_ebp=0x6789;
  sub_00166AB0();
  assert(esp==STACK+4 && ebx==0x9876 && esi==0x6543 && edi==0x1234);
  for(unsigned i=0;i<sizeof(before);++i){
   assert(memory[physics+i]==before[i]);
  }
  assert(kicks==(!player && seat==1 && vehicle!=0));
  assert(disconnects==(!anim&&!player));assert(eliminates==disconnects);
  assert(MEM8(MANAGER+0x8ca)==(seat==2?3:4));++cases;
 }
 printf("%u driver-death cases: retail latched controls preserved; driver kick-out, other seats/types/player, death animation and ABI pass\n",cases);
}
"""
    with tempfile.TemporaryDirectory(prefix='merc-tank-death-') as td:
        td=Path(td);(td/'test.c').write_text(pre+death+main)
        exe=td/'test.exe'
        subprocess.run(['C:/MinGW/bin/gcc.exe','-O1','-fno-strict-aliasing','-I'+str(gen.parent),str(td/'test.c'),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True)
