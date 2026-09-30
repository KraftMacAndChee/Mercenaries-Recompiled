"""Execute the actual generated traffic selector against original cap rules."""
from pathlib import Path
import re, subprocess, tempfile
ROOT = Path(__file__).resolve().parents[2]
s = (ROOT / "ports/mercenaries/src/recomp/gen/recomp_0006.c").read_text()
body = re.search(r"void sub_00169410\(void\)\n\{.*?\n\}", s, re.S)[0]
pre = r"""
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
static uint32_t mem[0x310000/4],eax,ebx,ecx,edx,esi,edi,esp;
#define MEM32(a) mem[(a)/4]
#define RECOMP_TRACE_FUNC(a) ((void)0)
#define PUSH32(s,v) do{uint32_t x=(v);s-=4;MEM32(s)=x;}while(0)
#define POP32(s,v) do{v=MEM32(s);s+=4;}while(0)
#define CMP_BE(a,b) ((uint32_t)(a)<=(uint32_t)(b))
#define CMP_AE(a,b) ((uint32_t)(a)>=(uint32_t)(b))
#define CMP_B(a,b) ((uint32_t)(a)<(uint32_t)(b))
#define TEST_Z(a,b) (((a)&(b))==0)
#define LO8(a) ((uint8_t)(a))
#define SET_LO8(a,b) ((a)=((a)&0xffffff00u)|(uint8_t)(b))
"""
main = r"""
static uint32_t seed=53;
static uint32_t random32(void){seed=seed*1664525u+1013904223u;return seed;}
static int expected(uint32_t n,uint32_t *next){
 uint32_t valid[15],k=0;
 for(uint32_t i=0;i<n;i++)if(MEM32(0x1084+i*4)<MEM32(0x1048+i*4))valid[k++]=i;
 if(!k)return -1;
 uint32_t r=*next;
 r=(r>>1)^((((r^(r>>1))&1)?0x8b6724b5u:0)+0x98ce4424u);
 uint32_t rot=(r<<19)|(r>>13);*next=rot;
 return valid[(uint32_t)(r+rot)%k];
}
int main(void){
 for(unsigned c=0;c<200000;c++){
  uint32_t n=c%16;MEM32(0x1008)=n;
  for(uint32_t i=0;i<15;i++){
   uint32_t cap=random32()%8,count=random32()%10;
   if(c%7==0)count=cap;
   if(c%11==0)count=0xffffffffu;
   if(c%13==0)cap=0xffffffffu;
   MEM32(0x1048+i*4)=cap;MEM32(0x1084+i*4)=count;
  }
  uint32_t next=random32();MEM32(0x30eff0)=next;
  int want=expected(n,&next);
  ecx=0x1000;esp=0x2000;ebx=0x12345678;esi=0x23456789;edi=0x3456789a;
  sub_00169410();
  assert((int32_t)eax==want&&MEM32(0x30eff0)==next);
  assert(esp==0x2004&&ebx==0x12345678&&esi==0x23456789&&edi==0x3456789a);
 }
 puts("PASS: 200000 production selector cases; empty/full/overflow caps; RNG and guest ABI preserved");
}
"""
with tempfile.TemporaryDirectory(prefix="traffic-selector-") as d:
 p=Path(d);(p/"test.c").write_text(pre+body+main)
 subprocess.run(["C:/MinGW/bin/gcc.exe","-O2",str(p/"test.c"),"-o",str(p/"test.exe")],check=True)
 subprocess.run([str(p/"test.exe")],check=True)
