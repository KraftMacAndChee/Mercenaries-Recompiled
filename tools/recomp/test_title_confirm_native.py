"""Execute the generated title handler with the rebindable menu event IDs."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'ports/mercenaries/src/recomp/gen/recomp_0004.c').read_text()
a=s.index('void sub_000D4970(void)');b=s.index('\n/**',a)
handler=s[a:b]
pre=r"""
#include <stdint.h>
#include <stdio.h>
#include <assert.h>
static unsigned char memory[0x450000];
static uint32_t eax,ecx,edx,esp;static int selected;
#define MEM32(a) (*(uint32_t*)(memory+(a)))
#define MEM8(a) (memory[(a)])
#define LO8(a) ((a)&255)
#define SET_LO8(a,b) ((a)=((a)&~255u)|((b)&255))
#define CMP_NE(a,b) ((a)!=(b))
#define TEST_NZ(a,b) (((a)&(b))!=0)
#define PUSH32(s,v) ((s)-=4,MEM32(s)=(v))
#define RECOMP_TRACE_FUNC(a) ((void)0)
static void recomp_frontend_input_checkpoint(uint32_t m,uint32_t i,uint32_t e){}
static void sub_000D3EA0(void){assert(MEM32(esp+4)==1);eax=0x14000;esp+=8;}
static void sub_000D3E60(void){assert(MEM32(esp+4)==0x14000);++selected;esp+=8;}
"""
main=r"""
int main(void){
 for(unsigned input=0;input<8;++input)for(unsigned event=0;event<3;++event){
  esp=0x10000;ecx=0x12000;MEM32(ecx+0x4c)=0x13000;selected=0;
  MEM32(esp)=0xabcdef;MEM32(esp+4)=input;MEM32(esp+8)=event;
  sub_000D4970();assert(esp==0x1000c);
  assert(selected==((input==5 || input==7) && event==1));
 }
 puts("title: custom Menu Confirm and Start accepted; other events ignored; ret-8 ABI preserved");
}
"""
with tempfile.TemporaryDirectory(prefix='merc-title-') as folder:
 d=Path(folder);(d/'test.c').write_text(pre+handler+main);exe=d/'test.exe'
 subprocess.run(['C:/MinGW/bin/gcc.exe','-std=c11','-O2',str(d/'test.c'),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
