"""Run the production streaming admission function against bounded queue cases."""
from pathlib import Path
import subprocess,shutil,tempfile
ROOT=Path(__file__).resolve().parents[2]
s=(ROOT/'ports/mercenaries/src/recomp/gen/recomp_0011.c').read_text(encoding='utf-8')
a=s.index('void sub_00222190(void)\n{');body=s[a:s.index('\n}',a)+2]
assert 'recomp_stream_admit_idle' in body
header=r"""
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "stream_admission.h"
static unsigned char mem[0x100000];
static uint32_t eax,ecx,edx,esp,ebx,esi,edi,g_seh_ebp,g_esp;
static int allocations,pushes;
#define MEM32(a) (*(uint32_t*)(mem+(uint32_t)(a)))
#define MEM8(a) mem[(uint32_t)(a)]
#define LO8(a) ((uint8_t)(a))
#define SET_LO8(a,v) ((a)=((a)&0xffffff00)|((v)&255))
#define PUSH32(s,v) do{(s)-=4;MEM32(s)=(v);}while(0)
#define POP32(s,v) do{(v)=MEM32(s);(s)+=4;}while(0)
#define RECOMP_TRACE_FUNC(a) ((void)0)
#define TEST_NZ(a,b) (((a)&(b))!=0)
#define TEST_Z(a,b) (((a)&(b))==0)
#define CMP_EQ(a,b) ((a)==(b))
#define CMP_GE(a,b) ((int32_t)(a)>=(int32_t)(b))
int recomp_stream_admit_idle(uint32_t m,uint32_t n){return recomp_stream_idle_admission(MEM32(m+0x3b4),n,MEM32(m+0x218),(MEM8(m+0x10c)!=0)||(MEM32(m+0x110)!=MEM32(m+0x114)));}
#define RECOMP_ICALL_SAFE(fn,sp) do{assert((fn)==123);allocations++;eax=0x50000;esp+=4;}while(0)
void sub_00221D30(void){pushes++;esp+=8;}
"""
tail=r"""
static void check(uint32_t used,uint32_t bytes,int active,int queued,int expect){
 memset(mem,0,sizeof(mem));const uint32_t m=0x10000,req=0x20000;
 MEM32(m+8)=1;MEM32(m+12)=req;MEM32(m+0x3b0)=1;MEM32(m+0x3b4)=used;MEM32(m+0x3c0)=123;
 MEM32(m+0x218)=active;MEM32(m+0x114)=queued;MEM32(req+0x24)=bytes/2048-1;
 ecx=m;ebx=111;esi=222;edi=333;g_seh_ebp=444;esp=g_esp=0xf0000;allocations=pushes=0;
 sub_00222190();assert(allocations==expect&&pushes==expect);assert(esp==0xf0004&&ebx==111&&esi==222&&edi==333);
 assert(MEM32(m+4)==(uint32_t)expect);assert(MEM32(m+0x3b4)==used+(expect?bytes:0));
 assert(MEM32(m+0x3b0)==1+(uint32_t)expect);
 if(expect){assert(MEM32(req+12)==0x50000&&MEM32(req+0x14)==bytes&&MEM32(req+0x18)==1);}
}
int main(void){
 check(126976,555008,0,0,1); /* captured idle NW loading deadlock */
 check(126976,555008,1,0,0);check(126976,555008,0,1,0);
 check(512000,555008,0,0,0);check(511999,2048,0,0,1);
 check(126976,2048,1,1,1);check(511999,2048,1,0,0);
 assert(!recomp_stream_idle_admission(1,0,0,0));
 assert(!recomp_stream_idle_admission(1,64u*1024*1024,0,0));
 for(uint32_t n=0;n<1048576;n+=997)for(uint32_t b=2048;b<1048576;b+=8192){
  int admit=recomp_stream_idle_admission(n,b,0,0);
  assert(!admit||n<512000);assert(!recomp_stream_idle_admission(n,b,1,0));assert(!recomp_stream_idle_admission(n,b,0,1));
 }
 puts("PASS: production streaming queue/register/accounting cases and bounded idle admission oracle");
}
"""
with tempfile.TemporaryDirectory(prefix='merc-stream-') as d:
 c=Path(d)/'test.c';exe=Path(d)/'test.exe';c.write_text(header+body+tail,encoding='utf-8')
 subprocess.run([shutil.which('gcc') or 'C:/MinGW/bin/gcc.exe','-O2','-std=c11','-I',str(ROOT/'ports/mercenaries/src'),str(c),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True,timeout=10)
