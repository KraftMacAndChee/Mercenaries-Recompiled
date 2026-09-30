"""Exercise the lifted brush walker: skipped HUD cannot unbalance paint scopes."""
from pathlib import Path
import re,subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'ports/mercenaries/src/recomp/gen/recomp_0010.c').read_text()
body=re.search(r'void sub_0020B810\(void\)\n\{.*?\n\}',s,re.S)[0]
pre=r"""
#include <stdint.h>
#include <assert.h>
#include <stdio.h>
static unsigned char ram[0x7ac000];
#define MEM32(a) (*(uint32_t*)(ram+(a)))
#define PUSH32(s,v) ((s)-=4,MEM32(s)=(v))
#define POP32(s,v) ((v)=MEM32(s),(s)+=4)
#define TEST_Z(a,b) (((a)&(b))==0)
#define TEST_NZ(a,b) (((a)&(b))!=0)
#define RECOMP_TRACE_FUNC(x) ((void)0)
static uint32_t eax,ecx,edx,esi,g_esp;
#define esp g_esp
static int hide,begins,ends,paints,depth;
int recomp_controls_hide_hud_brush(uint32_t b,uint32_t canvas){return hide && canvas==0x364028;}
void recomp_ui_begin_brush(uint32_t b){assert(!depth);++depth;}
void recomp_ui_end_brush(void){assert(depth==1);--depth;}
static void call(uint32_t address){
 if(address==1){assert(!depth);++begins;}
 else if(address==2){assert(depth==1);++paints;}
 else if(address==3){assert(depth==1);++ends;}
 else assert(0);
 esp+=4;
}
#define RECOMP_ICALL_SAFE(a,s) call(a)
"""
post=r"""
int main(void){
 const uint32_t list=0x10000,n1=0x10100,n2=0x10200,end=0x10300,b1=0x11000,b2=0x11100,vt=0x12000;
 MEM32(list)=n1;MEM32(n1)=n2;MEM32(n2)=end;
 MEM32(n1+8)=b1;MEM32(n2+8)=b2;MEM32(end+8)=0;
 MEM32(b1)=MEM32(b2)=vt;MEM32(b1+4)=0x364028;MEM32(b2+4)=0x342F30;
 MEM32(vt+0x18)=1;MEM32(vt+0x14)=2;MEM32(vt+0x1c)=3;
 for(hide=0;hide<=1;++hide){
  esp=0x20000;ecx=list;esi=0xdeadbeef;begins=ends=paints=depth=0;
  sub_0020B810();assert(esp==0x20004 && esi==0xdeadbeef);
  assert(begins==2-hide && ends==2-hide && paints==2-hide && !depth);
  assert(MEM32(0x7ab60c)==0 && MEM32(n1)==n2 && MEM32(n2)==end);
 }
 puts("HUD brush walker: hidden HUD skipped; other UI drawn; guest stack, list and paint scope preserved");
}
"""
with tempfile.TemporaryDirectory(prefix='merc-hud-') as folder:
 d=Path(folder);(d/'test.c').write_text(pre+body+post)
 subprocess.run(['C:/MinGW/bin/gcc.exe','-std=c11','-O2',str(d/'test.c'),'-o',str(d/'test.exe')],check=True)
 subprocess.run([str(d/'test.exe')],check=True)
