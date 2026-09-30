"""Run both production NPC boarding paths against reachable and airborne docks."""
from pathlib import Path
import re,subprocess,tempfile
ROOT=Path(__file__).resolve().parents[2]
gen=(ROOT/'ports/mercenaries/src/recomp/gen/recomp_0002.c').read_text()
addresses=['000843F0','00086610']
functions='\n'.join(re.search(r'void sub_'+a+r'\(void\)\n\{.*?\n\}',gen,re.S)[0] for a in addresses)
source='#define RECOMP_GENERATED_CODE\n#include "'+(ROOT/'ports/mercenaries/src/recomp/recomp_types.h').as_posix()+'"\n'+r"""
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
uint32_t g_eax,g_ecx,g_edx,g_esp,g_ebx,g_esi,g_edi,g_seh_ebp,g_fp_top;
volatile uint32_t g_recomp_current_func;
uint16_t g_x87_control_word,g_x87_status_word;
uint64_t g_mm0,g_mm1,g_mm2,g_mm3,g_mm4,g_mm5,g_mm6,g_mm7;
ptrdiff_t g_xbox_mem_offset;double g_fp_stack[8];
float g_xmm0[4],g_xmm1[4],g_xmm2[4],g_xmm3[4],g_xmm4[4],g_xmm5[4],g_xmm6[4],g_xmm7[4];
static unsigned char memory[0x400000];
enum {SEAT=0x10000,MANAGER=0x12000,VEHICLE=0x14000,STATE=0x16000,RIDER=0x18000,AI=0x1a000,VT=0x20000};
static float dock_y,dock_x;
static int received;
static int og_bugs;
int recomp_options_og_bugs(void){return og_bugs;}
static void *guest_ptr(uint32_t a){return memory+a;}
static uint32_t guest_u32(uint32_t a){return MEM32(a);}
static int dev_guest_address(uint32_t a,uint32_t n){return a>=0x10000 && a<=sizeof(memory)-n;}
"""+'\n#include "'+(ROOT/'ports/mercenaries/src/ai_boarding.h').as_posix()+'"\n'+r"""
static void indirect(uint32_t a){
 switch(a){
 case 1:eax=1;break;
 case 2:eax=1;break;
 case 3:eax=MANAGER;break;
 case 4:eax=RIDER;break;
 case 5:{unsigned out=MEM32(esp+4);memcpy(memory+out,memory+RIDER+0xe0,12);eax=out;esp+=4;break;}
 default:fprintf(stderr,"Unexpected icall %x\n",a);abort();
 }esp+=4;
}
#undef RECOMP_TRACE_FUNC
#define RECOMP_TRACE_FUNC(va) ((void)0)
#undef RECOMP_ICALL_SAFE
#define RECOMP_ICALL_SAFE(va,saved) indirect(va)
"""
custom={
'00162030':'eax=0;esp+=4;',
'001636F0':'eax=0;esp+=8;',
'001665B0':'unsigned out=MEM32(esp+4);MEMF(out)=dock_x;MEMF(out+4)=dock_y;MEMF(out+8)=0;eax=1;esp+=12;',
'00083360':'eax=fabsf(dock_x)>=1.1f;esp+=20;',
'00167000':'assert(MEM32(esp+4)==RIDER && MEM32(esp+8)==SEAT);++received;eax=1;esp+=20;',
'00168C00':'assert(MEM32(esp+4)==RIDER);++received;eax=1;esp+=16;',
'00162000':'eax=SEAT;esp+=8;',
'00170730':'eax=VEHICLE;esp+=4;',
}
calls=set(re.findall(r'\b(sub_[0-9A-F]{8})\(\)',functions))-{f'sub_{a}' for a in addresses}
for f in sorted(calls):source+='void '+f+'(void){'+custom.get(f[4:],'fprintf(stderr,"Unexpected '+f+'\\n");abort();')+'}\n'
source+=functions+r"""
static void reset(int helicopter,int driver,float height){
 memset(memory,0,sizeof(memory));g_xbox_mem_offset=(ptrdiff_t)memory;
 esp=0x3e0000;ebx=0x11111111;esi=0x22222222;edi=0x33333333;g_seh_ebp=0x44444444;
 MEM32(SEAT)=MANAGER;MEM32(MANAGER)=VEHICLE;MEM32(VEHICLE)=helicopter?0x2E21A8:VT+0x400;
 MEM32(SEAT+0x114)=driver?1:2;MEM32(AI)=VT;
 MEM32(VT+0x30)=4;MEM32(VT+0x3c)=5;
 unsigned v=MEM32(VEHICLE);MEM32(v+0x158)=1;MEM32(v+4)=2;MEM32(v+0x208)=3;
 MEM32(STATE+8)=AI;MEM32(STATE+0xc)=VEHICLE;MEM32(STATE+0x14)=1;
 MEM32(0x30D354)=1;MEM32(0x30D358)=1;
 MEMF(0x2DC08C)=1;MEMF(0x2DC3F0)=1.5f;MEMF(0x2DD414)=.1f;MEMF(0x2E4268)=6.25f;
 MEMF(0x2DC340)=.5f;MEMF(0x2DC3B8)=1.1f;MEMF(0x2E5CC4)=1.21f;
 dock_y=height;dock_x=0;received=0;
}
static void run(int state){
 esp=0x3e0000;
 if(state){ecx=STATE;sub_00086610();assert(esp==0x3e0008);}
 else{ecx=AI;MEM32(esp+4)=SEAT;MEM32(esp+8)=1;sub_000843F0();assert(esp==0x3e000c);}
 assert(ebx==0x11111111 && esi==0x22222222 && edi==0x33333333 && g_seh_ebp==0x44444444);
}
int main(void){
 const float heights[]={0,.5f,1.f,1.1f,1.11f,12,-12,NAN};
 for(og_bugs=0;og_bugs<=1;++og_bugs)for(int state=0;state<2;++state)for(int heli=0;heli<2;++heli)for(int driver=0;driver<2;++driver){
  for(unsigned h=0;h<sizeof(heights)/sizeof(heights[0]);++h){
   reset(heli,driver,heights[h]);run(state);
   int allowed=og_bugs || !heli || (isfinite(heights[h]) && fabsf(heights[h])<=1.1f);
   assert(received==allowed);assert((state?MEM32(AI+0x1c):eax)==(allowed?0x103:2));
  }
  reset(heli,driver,0);dock_x=10;run(state);assert(!received);
  reset(heli,driver,12);run(state);
  if(heli && !og_bugs){assert(!received);dock_y=.5f;run(state);assert(received==1);}
 }
 /* The applied option is read per attempt, including an NPC already waiting. */
 for(int state=0;state<2;++state)for(int driver=0;driver<2;++driver){
  og_bugs=0;reset(1,driver,100);run(state);assert(!received);
  og_bugs=1;run(state);assert(received==1);
  og_bugs=0;reset(1,driver,100);run(state);assert(!received);
  dock_y=.5f;run(state);assert(received==1);
 }
 puts("PASS: OG Bugs on/off and live switching; both retail AI boarding paths; driver/passenger; height and distance; wait then land; ordinary vehicles and guest registers preserved");
}
"""
with tempfile.TemporaryDirectory(prefix='ai-board-') as td:
 p=Path(td);(p/'test.c').write_text(source);exe=p/'test.exe'
 subprocess.run(['C:/MinGW/bin/gcc.exe','-std=c11','-O1','-fno-strict-aliasing','-I'+str(ROOT/'ports/mercenaries/src'),str(p/'test.c'),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
