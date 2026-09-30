"""Check crouch-transition detection and the generated carry-state dispatch."""
from pathlib import Path
import re
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[2]
gen = (ROOT / 'ports/mercenaries/src/recomp/gen/recomp_0001.c').read_text()
addresses=['0004C7D0','0004C7F0','00052DC0','00053630','00056820','0004CFA0']
function='\n'.join(re.search(r'void sub_'+a+r'\(void\)\n\{.*?\n\}',gen,re.S)[0] for a in addresses)
source = '#define RECOMP_GENERATED_CODE\n#include "' + (ROOT / 'ports/mercenaries/src/recomp/recomp_types.h').as_posix() + '"\n' + r"""
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
uint32_t g_eax,g_ecx,g_edx,g_esp,g_ebx,g_esi,g_edi,g_seh_ebp,g_fp_top;
volatile uint32_t g_recomp_current_func;
ptrdiff_t g_xbox_mem_offset;
static unsigned char memory[0x400000];
enum {PLAYER=0x10000,TARGET=0x12000,ANIM=0x14000,VT=0x18000,STATEVT=0x19000,STACK=0x3e0000};
static uint32_t guest_u32(uint32_t p){return MEM32(p);}
static uint8_t guest_u8(uint32_t p){return MEM8(p);}
static float guest_f32(uint32_t p){return MEMF(p);}
static int entered,completed,ended,puppet,started,hidden;
""" + '#include "' + (ROOT / 'ports/mercenaries/src/carry_pickup_guest.h').as_posix() + '"\n' + r"""
void sub_0004C7D0(void);void sub_0004C7F0(void);void sub_00052DC0(void);void sub_00053630(void);void sub_00056820(void);
void sub_000976E0(void){esp+=8;} /* Restore camera. */
void sub_001592B0(void){assert(MEM32(esp+4)==0);esp+=4;} /* cdecl letterbox. */
void sub_0017A0A0(void){assert(MEM32(esp+4)==0x4249d707 && MEM32(esp+8)==0xc2cbd863);esp+=4;}
void sub_0005CA70(void){assert(ecx==PLAYER && MEM32(esp+4)==1);esp+=8;}
void sub_0004CAF0(void){assert(ecx==PLAYER && MEM32(esp+4)==0x7013d630);esp+=8;}
void sub_0004E9D0(void){esp+=8;}
void sub_0004CB90(void){esp+=8;}
void sub_00061E50(void){abort();} /* Target has no animation object in this fixture. */
static void indirect(uint32_t address){
 if(address==1){
  assert(ecx==PLAYER+0x738 && MEM32(esp+4)==PLAYER && MEM32(esp+8)==TARGET);
  assert(MEM32(PLAYER+0x760)==PLAYER+0x738);entered++;
  /* Enter replaces the crouch animation: the decision must precede it. */
  MEM32(PLAYER+0x738+0xc)=TARGET;MEM32(ANIM+0x1964+0xb50)=0x12345678;esp+=12;
 }else if(address==2){
  assert(entered==1 && ecx==PLAYER && MEM32(esp+4)==0);completed++;sub_00056820();
 }else if(address==3){sub_00053630();
 }else if(address==4){assert(ecx==PLAYER);ended++;esp+=4;
 }else if(address==5){eax=0;esp+=4;
 }else if(address==6){assert(ecx==TARGET);puppet++;esp+=8;
 }else if(address==7){assert(ecx==TARGET);started++;esp+=8;
 }else if(address==8){eax=0x20000;esp+=4;
 }else if(address==9){assert(ecx==PLAYER && MEM32(esp+4)==0);hidden++;esp+=8;
 }else if(address==10){assert(ecx==PLAYER+0x830 && MEM32(esp+4)==PLAYER);esp+=8;
 }else abort();
}
#undef RECOMP_TRACE_FUNC
#define RECOMP_TRACE_FUNC(va) ((void)0)
#undef RECOMP_ICALL_SAFE
#define RECOMP_ICALL_SAFE(va,saved) do{indirect(va);}while(0)
""" + function + r"""
static void reset(void){
 memset(memory,0,sizeof(memory));g_xbox_mem_offset=(ptrdiff_t)memory;
 MEM32(PLAYER)=VT;MEM32(VT+0x334)=2;MEM32(PLAYER+4)=0x660E4490;
 MEM32(PLAYER+0x738)=STATEVT;MEM32(STATEVT+0x14)=1;MEM32(STATEVT+8)=3;
 MEM32(VT+0x2b0)=4;MEM32(VT+4)=5;MEM32(TARGET)=VT;
 MEM32(VT+0x33c)=6;MEM32(VT+0x2ac)=7;MEM32(VT+0x3bc)=8;MEM32(VT+0x290)=9;
 MEM32(PLAYER+0x830)=0x1a000;MEM32(0x1a000)=10;
 MEM32(PLAYER+0x6b0)=ANIM;MEM8(TARGET+0x6b9)=1;MEM32(ANIM+0x28e8)=1;
 MEM32(ANIM+0x1964+0xb50)=0xd0993b47;MEMF(ANIM+0x1964+0xb38)=.5f;
 esp=STACK;ecx=PLAYER;esi=0xabc;edi=0xdef;ebx=0x123;MEM32(esp+4)=TARGET;MEM32(esp+8)=0;
 entered=completed=ended=puppet=started=hidden=0;
}
static void run(int skip){
 assert(recomp_crouch_pickup_transition(PLAYER,TARGET)==skip);
 sub_0004CFA0();assert(entered==1 && completed==skip);
 assert(esp==STACK+12 && esi==0xabc && edi==0xdef && ebx==0x123);
 assert(ended==skip && puppet==skip && started==skip && hidden==skip);
 if(skip){assert(MEM32(PLAYER+0x79c)==TARGET);assert(MEM32(PLAYER+0x760)==PLAYER+0x830);}
}
int main(void){
 const float weights[]={0,.2f,.5f,.999f,1,-.1f,NAN};
 for(unsigned i=0;i<7;i++){reset();MEMF(ANIM+0x1964+0xb38)=weights[i];run(i<4);}
 reset();MEM32(ANIM+0x1964+0xb50)=0xc26a444e;run(0);
 reset();MEM32(PLAYER+4)=0;run(0);
 reset();MEM8(TARGET+0x6b9)=0;run(0);
 reset();MEM32(PLAYER+0x79c)=TARGET;run(0);
 reset();MEM32(PLAYER+0x6b4)=1;run(0);
 reset();MEM32(ANIM+0x28e8)=0;MEM32(ANIM+0xe10+0xb50)=0xda4f821b;MEMF(ANIM+0xe10+0xb38)=.3f;run(1);
 reset();MEM32(PLAYER+0x6b0)=0;run(0);
 reset();assert(!recomp_crouch_pickup_transition(0,TARGET));assert(!recomp_crouch_pickup_transition(PLAYER,0));
 puts("PASS: crouch-only carry completion, normal pickups, eligibility guards and guest ABI");
}
"""
with tempfile.TemporaryDirectory(prefix='crouch-pickup-') as directory:
    path=Path(directory);(path/'test.c').write_text(source);exe=path/'test.exe'
    subprocess.run(['C:/MinGW/bin/gcc.exe','-std=c11','-O1','-fno-strict-aliasing','-I'+str(ROOT/'ports/mercenaries/src'),str(path/'test.c'),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
