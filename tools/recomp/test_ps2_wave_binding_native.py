"""Check the generated XACT buffer-binding ABI and both HRESULT failure paths."""
from pathlib import Path
import re, subprocess, tempfile
ROOT=Path(__file__).resolve().parents[2]
s=(ROOT/'ports/mercenaries/src/recomp/gen/recomp_0012.c').read_text(encoding='utf-8')
body=re.search(r'void sub_002821ED\(void\)\n\{.*?\n\}',s,re.S)[0]
prelude=r'''#include <stdint.h>
#include <assert.h>
#include <stdio.h>
static uint8_t memory[65536];
static uint32_t esp,eax,ecx,replacement;
static int data_calls,region_calls,retail_calls,fail_data,fail_region;
#define MEM32(a) (*(uint32_t*)(memory+(uint32_t)(a)))
#define PUSH32(s,v) do { uint32_t tmp=(v); s-=4; MEM32(s)=tmp; } while(0)
#define TEST_S(a,b) ((int32_t)((a)&(b))<0)
#define RECOMP_TRACE_FUNC(a) ((void)0)
static uint32_t recomp_ps2_wave_data(uint32_t p) { assert(p==0x2000); return replacement; }
static void sub_00280FD5(void) {
 assert(ecx==0x1000 && MEM32(esp+4)==replacement && MEM32(esp+8)==1234);
 ++data_calls; eax=fail_data?0x8007000e:0; esp+=12;
}
static void sub_002821D5(void) {
 assert(MEM32(esp+4)==0x1000); ++retail_calls; eax=fail_data?0x8007000e:0;esp+=8;
}
static void sub_0028100F(void) {
 assert(ecx==0x1000 && MEM32(esp+4)==(replacement?0:80) && MEM32(esp+8)==1234);
 ++region_calls; eax=fail_region?0x80004005:0;esp+=12;
}
'''
main=r'''
int main(void) {
 for(int custom=0;custom<2;custom++)for(fail_data=0;fail_data<2;fail_data++)for(fail_region=0;fail_region<2;fail_region++) {
  replacement=custom?0x3000:0; data_calls=retail_calls=region_calls=0;
  esp=0x8000;MEM32(esp+4)=0x1000;MEM32(esp+8)=0x2000;MEM32(0x2008)=80;MEM32(0x200c)=1234;
  sub_002821ED();assert(esp==0x800c && data_calls==custom && retail_calls==!custom && region_calls==!fail_data);
  assert(eax==(fail_data?0x8007000e:fail_region?0x80004005:0));
 }
 puts("PASS: generated PS2/retail buffer binding, stack balance, play region, data/region failures");
}
'''
with tempfile.TemporaryDirectory(prefix='merc-ps2-binding-') as folder:
 p=Path(folder); (p/'test.c').write_text(prelude+body+main)
 subprocess.run(['C:/MinGW/bin/gcc.exe','-std=c11','-O2',str(p/'test.c'),'-o',str(p/'test.exe')],check=True)
 subprocess.run([str(p/'test.exe')],check=True)
