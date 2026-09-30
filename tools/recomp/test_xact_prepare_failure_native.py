"""A failed XACT Prepare must return its reserved positional source and handle."""
from pathlib import Path
import re,subprocess,tempfile
ROOT=Path(__file__).resolve().parents[2]
s=(ROOT/'ports/mercenaries/src/recomp/gen/recomp_0011.c').read_text()
def function(name):return re.search(r'void '+name+r'\(void\)\n\{.*?\n\}',s,re.S)[0]
prelude=r'''
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
static uint8_t memory[0x100000];
static uint32_t eax,ecx,edx,esi,edi,esp,g_seh_ebp;
static int fail,free_slot,live,releases,diagnostics,properties;
#define MEM32(a) (*(uint32_t*)(memory+(uint32_t)(a)))
#define MEM8(a) memory[(uint32_t)(a)]
#define LO8(a) ((uint8_t)(a))
#define SET_LO8(a,b) ((a)=((a)&0xffffff00u)|(uint8_t)(b))
#define PUSH32(s,v) do{uint32_t t=(v);s-=4;MEM32(s)=t;}while(0)
#define POP32(s,v) do{v=MEM32(s);s+=4;}while(0)
#define CMP_GE(a,b) ((int32_t)(a)>=(int32_t)(b))
#define CMP_LE(a,b) ((int32_t)(a)<=(int32_t)(b))
#define TEST_S(a,b) ((int32_t)((a)&(b))<0)
#define TEST_NZ(a,b) (((a)&(b))!=0)
#define RECOMP_TRACE_FUNC(a) ((void)0)
#define recomp_xact_cue_lifetime_checkpoint(a,b,c) ((void)0)
/* Subscription lifecycle is exercised with the real pool by test_xact_stop_recovery_native. */
void recomp_xact_forget_subscription(uint32_t w){}
static void sub_0027D9CE(void){
 uint32_t out=MEM32(esp+12);MEM32(out)=fail?0:0x40000;eax=fail?0x8007000e:0;esp+=16;
}
static void sub_00225570(void){properties++;esp+=8;}
static void sub_00226510(void){assert(ecx==0x10000+0x7568);assert(MEM32(esp+4)==123);releases++;live--;esp+=8;}
void recomp_xact_prepare_failure(uint32_t m,uint32_t w,uint32_t r){assert(m==0x10000&&w==0x30000&&(int32_t)r<0);diagnostics++;}
'''
main=r'''
int main(void){
 for(fail=0;fail<=1;fail++)for(int manual=0;manual<2;manual++)for(int source=-1;source<64;source++){
  memset(memory,0,sizeof(memory));releases=diagnostics=properties=0;live=1;
  uint32_t m=0x10000,w=0x30000;MEM32(w+4)=123;MEM32(w+12)=source;MEM32(w+20)=0x35000;MEM32(0x35008)=0x36000;
  MEM32(w+24)=42;MEM32(w+40)=0x3fc00000;MEM8(w+48)=manual;MEM32(m+0x7564)=64;
  if(source>=0)MEM8(m+0x7524+source)=1;
  esi=0xabc;edi=0xdef;ecx=m;esp=0x80000;MEM32(esp+4)=w;MEM32(esp+8)=0x70000;
  sub_002258E0();assert(esp==0x8000c&&esi==0xabc&&edi==0xdef);
  assert(releases==fail&&diagnostics==fail&&properties==!fail&&live==!fail);
  assert(eax==(fail?0:123));assert(MEM32(w)==(fail?0:manual+1));
  if(source>=0){assert(MEM8(m+0x7524+source)==!fail);assert(MEM32(m+0x7564)==(fail?source:64));}
 }
 /* A long burst repeatedly reuses one slot; it must never reach 64 leaked sources. */
 fail=1;for(int i=0;i<512;i++){
  uint32_t m=0x10000,w=0x30000;MEM32(w+4)=123;MEM32(w+12)=0;MEM32(w+20)=0x35000;MEM32(0x35008)=0x36000;
  MEM32(m+0x7564)=1;MEM8(m+0x7524)=1;live=1;ecx=m;esp=0x80000;MEM32(esp+4)=w;MEM32(esp+8)=0x70000;
  sub_002258E0();assert(!live&&!MEM8(m+0x7524)&&!MEM32(m+0x7564));
 }
 puts("XACT Prepare failure: 260 state/source cases and 512 failures release slots; successful prepares unchanged");
}
'''
code=prelude+function('sub_00226830')+'\n'+function('sub_002258E0')+main
with tempfile.TemporaryDirectory(prefix='merc-xact-prepare-') as d:
 p=Path(d);(p/'test.c').write_text(code);subprocess.run(['C:/MinGW/bin/gcc.exe','-std=c11','-O2',str(p/'test.c'),'-o',str(p/'test.exe')],check=True);subprocess.run([str(p/'test.exe')],check=True)
 old=code.replace('    PUSH32(esp, esi);\n    ecx = edi;\n    PUSH32(esp, 0); sub_00226830(); /* release failed preparation */\n','')
 assert old!=code;(p/'old.c').write_text(old);subprocess.run(['C:/MinGW/bin/gcc.exe','-std=c11','-O2',str(p/'old.c'),'-o',str(p/'old.exe')],check=True);assert subprocess.run([str(p/'old.exe')],capture_output=True).returncode!=0
