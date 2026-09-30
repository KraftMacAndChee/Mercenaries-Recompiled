"""Exercise the lifted credits AddName default arm, including its shared stack frame."""
from pathlib import Path
import re, subprocess, tempfile
ROOT=Path(__file__).resolve().parents[2]
source="\n".join((ROOT/'ports/mercenaries/src/recomp/gen'/name).read_text() for name in ('recomp_0004.c','recomp_stubs_unresolved.c'))
functions=dict((name,body) for body,name in re.findall(r'(void (sub_[0-9A-F]{8})\(void\)\n\{.*?\n\})',source,re.S))
names=['sub_000E0AD0','sub_000DCED0','sub_000E0B58','sub_000E0B84','sub_000E0B5D']
fixture='#define RECOMP_GENERATED_CODE\n#include "'+(ROOT/'ports/mercenaries/src/recomp/recomp_types.h').as_posix()+'"\n'+r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
uint32_t g_eax,g_ecx,g_edx,g_esp,g_ebx,g_esi,g_edi,g_seh_ebp,g_fp_top;
ptrdiff_t g_xbox_mem_offset;
double g_fp_stack[8];
float g_xmm0[4],g_xmm1[4],g_xmm2[4],g_xmm3[4],g_xmm4[4],g_xmm5[4],g_xmm6[4],g_xmm7[4];
static unsigned char memory[0x400000];
#undef RECOMP_TRACE_FUNC
#define RECOMP_TRACE_FUNC(a) ((void)0)
#undef RECOMP_ITAIL
#define RECOMP_ITAIL(a) abort()
void sub_00012000(void){g_fp_stack[--g_fp_top&7u]=.25;esp+=4;}
'''+"\n".join('void '+n+'(void);' for n in names)+"\n"+"\n".join(functions[n] for n in names)+r'''
int main(void){
 g_xbox_mem_offset=(ptrdiff_t)memory;
 MEMF(0x2E7024)=35;MEMF(0x2DC8DC)=20;
 const unsigned brush=0x10000,stack=0x300000,text=0x20000;
 MEMF(brush+0x2C)=90;MEMF(brush+0x30)=170;MEM32(brush+0x274)=7;
 for(unsigned i=0;i<8;i++){
  esp=stack;ecx=brush;esi=0x12345678;ebx=0x98765432;edi=0x13579BDF;
  MEM32(stack)=0xABCDEF;MEM32(stack+4)=text+i*64;g_fp_top=3;
  sub_000E0AD0();
  assert(esp==stack+8 && esi==0x12345678 && ebx==0x98765432 && edi==0x13579BDF && g_fp_top==3);
  unsigned painter=brush+0xCC+i*0x34;
  assert(MEMF(painter+4)==125 && MEMF(painter+8)==170+(i+1)*20);
  assert(MEMF(painter+0xC)==125 && MEMF(painter+0x10)==170+(i+1)*20);
  assert(MEMF(painter+0x14)==0 && MEM32(painter+0x28)==text+i*64);
 }
 esp=stack;ecx=brush;esi=0x12345678;sub_000E0AD0();assert(esp==stack+8&&esi==0x12345678&&MEM32(brush+0x26C)==8);
 puts("PASS: stationary legal credits keep original positions/text, name capacity, saved registers, x87 depth and caller stack");
}
'''
with tempfile.TemporaryDirectory(prefix='merc-credits-') as d:
 p=Path(d);(p/'test.c').write_text(fixture)
 subprocess.run(['C:/MinGW/bin/gcc.exe','-std=c11','-O2','-fno-strict-aliasing',str(p/'test.c'),'-o',str(p/'test.exe')],check=True)
 subprocess.run([str(p/'test.exe')],check=True)
