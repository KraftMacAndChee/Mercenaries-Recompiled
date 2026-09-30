"""Run lifted traffic attach/detach accounting and real path/type lookups.

Actor->spore, permanent-name and squad calls are controlled dependencies.
This verifies counter/cooldown and guest ABI behavior, not world hibernation.
"""
from pathlib import Path
import re, subprocess, tempfile
ROOT=Path(__file__).resolve().parents[2]
s=(ROOT/'ports/mercenaries/src/recomp/gen/recomp_0006.c').read_text()
addresses=['00168E90','00168F00','00169E10','00169E90']
bodies='\n'.join(re.search(r'void sub_'+a+r'\(void\)\n\{.*?\n\}',s,re.S)[0] for a in addresses)
pre=r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
static uint8_t mem[0x30000];
static uint32_t eax,ebx,ecx,edx,esi,edi,esp;
static float xmm0v[4],xmm1v[4];
static unsigned squad_calls;
#define xmm0 xmm0v[0]
#define xmm1 xmm1v[0]
#define MEM32(a) (*(uint32_t *)(mem+(a)))
#define MEM8(a) mem[(a)]
#define MEMF(a) (*(float *)(mem+(a)))
#define RECOMP_TRACE_FUNC(a) ((void)0)
#define PUSH32(s,v) do{uint32_t x=(v);s-=4;MEM32(s)=x;}while(0)
#define POP32(s,v) do{v=MEM32(s);s+=4;}while(0)
#define CMP_EQ(a,b) ((a)==(b))
#define CMP_LE(a,b) ((int32_t)(a)<=(int32_t)(b))
#define CMP_L(a,b) ((int32_t)(a)<(int32_t)(b))
#define TEST_Z(a,b) (((a)&(b))==0)
#define TEST_S(a,b) ((int32_t)((a)&(b))<0)
#define LO8(a) ((uint8_t)(a))
#define SET_LO8(a,b) ((a)=((a)&0xffffff00u)|(uint8_t)(b))
static void recomp_xmm_loadss(float *v,uint32_t a){v[0]=MEMF(a);v[1]=v[2]=v[3]=0;}
static void recomp_xmm_zero(float *v){memset(v,0,16);}
static void sub_00113700(void){eax=MEM32(ecx+8);esp+=4;}
static void sub_001ED370(void){eax=MEM32(MEM32(esp+4)+0x28);esp+=4;}
static void sub_0006B9B0(void){
 assert(ecx==0x5000&&MEM32(esp+4)==123&&MEM32(esp+8)==456);squad_calls++;esp+=12;
}
static void recomp_traffic_detach_checkpoint(uint32_t a,uint32_t b,uint32_t c,uint32_t d){
 assert(a<=1&&b>=0x28b58&&b<=0x28b80&&c==0x5000&&d<=1);
}
'''
main=r'''
static void call(void (*fn)(void),uint32_t path,unsigned flag){
 ecx=0x10000;esp=0xf000;ebx=0x12345678;esi=0x23456789;edi=0x3456789a;
 MEM32(esp+4)=path;MEM32(esp+8)=0x5000;MEM32(esp+12)=flag;
 fn();assert(esp==0xf010&&ebx==0x12345678&&esi==0x23456789&&edi==0x3456789a);
}
int main(void){
 unsigned cases=0;
 const float cools[]={0,600,-1};const float timers[]={0,123,-1};
 for(unsigned variant=0;variant<5;variant++)for(unsigned index=0;index<15;index++)
 for(unsigned dead=0;dead<2;dead++)for(unsigned squad=0;squad<2;squad++)
 for(unsigned cool=0;cool<3;cool++)for(unsigned timer=0;timer<3;timer++)
 for(unsigned path_index=0;path_index<2;path_index++){
  memset(mem,0,sizeof(mem));squad_calls=0;
  uint32_t r=0x28b58+path_index*0x28,path=0x9000+path_index*0x1000;
  MEM32(0x28b54)=2;MEM32(0x28b58)=0x9000;MEM32(0x28b80)=0xa000;
  MEM32(r+4)=variant==1?0:0x2000;MEM32(r+8)=0x4000;
  MEM32(r+0xc)=7;MEM32(0x4004)=19;MEM32(0x226c)=11;
  MEM32(r+0x1c)=3;MEM32(0x2270)=5;MEMF(0x2278)=cools[cool];MEMF(r+0x20)=timers[timer];
  MEM32(0x5010)=variant==2?0:0x6000;MEM32(0x6008)=variant==3?0:0x7000;
  MEM32(0x7028)=variant==4?999:100+index;
  MEM32(0x2008)=15;MEM32(0x2288)=123;MEM32(0x228c)=456;
  for(unsigned i=0;i<15;i++){MEM32(0x200c+i*4)=100+i;MEM32(0x2048+i*4)=1;MEM32(0x2084+i*4)=2;}
  call(sub_00169E10,path,squad);
  assert(MEM32(r+0xc)==8&&MEM32(0x4004)==20&&MEM32(0x226c)==(variant==1?11:12));
  assert(squad_calls==(variant!=1&&squad));
  for(unsigned i=0;i<15;i++)assert(MEM32(0x2084+i*4)==2+(variant==0&&i==index));
  call(sub_00169E90,path,dead);
  assert(MEM32(r+0xc)==7&&MEM32(0x4004)==19&&MEM32(0x226c)==11);
  for(unsigned i=0;i<15;i++)assert(MEM32(0x2084+i*4)==2);
  unsigned counted=dead&&variant!=1&&cools[cool]>0;
  assert(MEM32(r+0x1c)==3+counted&&MEM32(0x2270)==5+counted);
  assert(MEMF(r+0x20)==(counted&&timers[timer]<=0?cools[cool]:timers[timer]));
  cases++;
 }
 printf("PASS: %u lifted attach/detach cases; real path/type lookup, missing zone/actor/spore/type, 15 type slots, destruction vs transfer, 600s cooldown preservation, squad args and guest ABI\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='traffic-accounting-') as d:
 p=Path(d);(p/'test.c').write_text(pre+bodies+main)
 subprocess.run(['C:/MinGW/bin/gcc.exe','-O2','-fno-strict-aliasing',str(p/'test.c'),'-o',str(p/'test.exe')],check=True,timeout=30)
 subprocess.run([str(p/'test.exe')],check=True,timeout=30)
