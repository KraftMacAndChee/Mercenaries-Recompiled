"""Execute the generated stale-event guard before retail mouse/action dispatch."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'ports/mercenaries/src/recomp/gen/recomp_0003.c').read_text()
start=s.index('    { int recomp_controls_event_current',s.index('void sub_000A3AD0(void)'))
block=s[start:s.index('    PUSH32(esp, ecx);',start)]
pre=r"""
#include <stdint.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>
static unsigned char memory[0x420000];
#define MEM32(a) (*(uint32_t*)(memory+(a)))
#define MEMF(a) (*(float*)(memory+(a)))
static uint32_t eax,esp;static int current,dispatched,mouse_calls;
int recomp_controls_event_current(uint32_t a,uint32_t b,uint32_t c,uint32_t d){
 assert(a==0x4249d707 && b==0xc2cbd863 && c==1 && d==0);return current;
}
static float recomp_controls_mouse_axis(uint32_t name,float v,float dt){++mouse_calls;return v;}
"""
main=r"""
int main(void){
 MEM32(0x413f6c)=0x4249d707;MEM32(0x413f68)=0xc2cbd863;MEM32(0x323acc)=1;MEM32(0x323ad0)=0;
 for(current=0;current<=1;++current){
  esp=0x10000;eax=0xdeadbeef;MEM32(esp)=0x12345678;MEM32(esp+4)=0x28217089;MEMF(esp+8)=1;
  dispatched=mouse_calls=0;event();
  assert(dispatched==current && mouse_calls==current);
  assert(esp==0x10000+(current?0:12));assert(eax==(current?0xdeadbeef:0));
  assert(MEM32(0x10000)==0x12345678 && MEM32(0x10004)==0x28217089 && MEMF(0x10008)==1);
 }
 puts("transition event: stale packet consumed before mouse/actions; guest ret-8 ABI preserved");
}
"""
with tempfile.TemporaryDirectory(prefix='merc-transition-') as folder:
 d=Path(folder);(d/'test.c').write_text(pre+'static void event(void){\n'+block+'\n++dispatched; }\n'+main)
 exe=d/'test.exe';subprocess.run(['C:/MinGW/bin/gcc.exe','-std=c11','-O2',str(d/'test.c'),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
