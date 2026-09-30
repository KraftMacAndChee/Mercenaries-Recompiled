"""Native retail boundary selection and AI threshold address preservation."""
from pathlib import Path
import re
import runpy
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from tools.recomp import config


class SseAddressTests(unittest.TestCase):
    def test_actual_boundary_loop_and_ai_return_block(self):
        repairs = runpy.run_path(str(ROOT / 'ports/mercenaries/scripts/Patch-Generated.py'))['SSE_ADDRESS_COMPARE_REPAIRS']
        config.configure_from_xbe(str(ROOT / 'game_files/mercenaries-retail/default.xbe'))
        raw = (ROOT / 'game_files/mercenaries-retail/default.xbe').read_bytes()
        texts = {unit: (ROOT / f'ports/mercenaries/src/recomp/gen/recomp_{unit}.c').read_text()
                 for unit in ('0002', '0006')}
        for site, va, encoded, before, after in repairs:
            offset = config.va_to_file_offset(va)
            self.assertEqual(raw[offset:offset + len(bytes.fromhex(encoded))], bytes.fromhex(encoded))
            self.assertEqual(sum(t.count(after) for t in texts.values()), 1)
        boundary = re.search(r'void sub_00149350\(void\)\n\{.*?\n\}', texts['0006'], re.S)[0]
        old = boundary
        for site, va, encoded, before, after in repairs[1:]:
            old = old.replace(after, before)
        boundary = boundary.replace('void sub_00149350', 'static void boundary_fixed')
        old = old.replace('void sub_00149350', 'static void boundary_old')
        ai = re.search(r'loc_000657B9: ;\n.*?\n\}', texts['0002'], re.S)[0]
        ai_old = ai.replace(repairs[0][4], repairs[0][3])
        ai = 'static void ai_fixed(void) {int _flags=0;\n' + ai
        ai_old = 'static void ai_old(void) {int _flags=0;\n' + ai_old
        prelude = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>
static unsigned char mem[0x400000];
static uint32_t eax,ebx,ecx,edx,esi,edi,esp,g_seh_ebp;
static float xmm0v[4],xmm1v[4],xmm2v[4];
#define xmm0 xmm0v[0]
#define xmm1 xmm1v[0]
#define xmm2 xmm2v[0]
#define MEM32(a) (*(uint32_t*)(mem+(uint32_t)(a)))
#define MEMF(a) (*(float*)(mem+(uint32_t)(a)))
#define SMEM16(a) (*(int16_t*)(mem+(uint32_t)(a)))
#define PUSH32(s,v) do{uint32_t t=(v);(s)-=4;MEM32(s)=t;}while(0)
#define POP32(s,v) do{(v)=MEM32(s);(s)+=4;}while(0)
#define recomp_xmm_loadss(v,a) ((v)[0]=MEMF(a))
#define CMP_L(a,b) ((int32_t)(a)<(int32_t)(b))
#define CMP_GE(a,b) ((int32_t)(a)>=(int32_t)(b))
#define HI8(a) (((a)>>8)&255u)
#define SET_HI8(a,v) ((a)=((a)&0xffff00ffu)|(((uint32_t)(v)&255u)<<8))
#define RECOMP_TRACE_FUNC(a) ((void)0)
static int EVEN_PARITY8(uint8_t v) {unsigned n=0;for(;v;v>>=1)n+=v&1;return !(n&1);}
'''
        constants = {va: struct.unpack_from('<I', raw, config.va_to_file_offset(va))[0]
                     for va in (0x2DC8A0, 0x2DC8D8)}
        harness = r'''
static uint32_t rng=193;
static uint32_t next(void) {rng=rng*1664525u+1013904223u;return rng;}
static uint32_t run_boundary(void(*fn)(void),unsigned n) {
 esp=0x30000;esi=11;edi=12;ebx=13;g_seh_ebp=14;
 MEM32(esp)=0;MEM32(esp+4)=n;MEM32(esp+8)=0x20000;MEM32(esp+12)=0x10000;
 fn();if(esp!=0x30010||esi!=11||edi!=12||ebx!=13)exit(2);return eax;
}
static uint32_t run_ai(void(*fn)(void),float value,float threshold,float wrong) {
 esp=0x30000;ebx=0x10000;esi=11;edi=12;
 MEM32(esp)=21;MEM32(esp+4)=22;MEM32(esp+8)=0x20000;
 MEMF(esp+0x28)=value;MEMF(ebx+0x2D4)=threshold;MEMF(0x202D4)=wrong;
 fn();if(esp!=0x30034||edi!=21||esi!=22||ebx!=0x20000)exit(3);return eax;
}
int main(void) {
 CONSTANTS
 unsigned cases=0,old_failures=0;
 for(unsigned n=0;n<=33;++n)for(unsigned trial=0;trial<160;++trial) {
  float minx=FLT_MAX,maxz=-FLT_MAX;uint32_t expected=0xffffffffu;
  for(unsigned p=0;p<40;++p) {
   MEMF(0x10000+12*p)=trial%4==0?0:(int)(next()%17)-8;
   MEMF(0x10004+12*p)=(float)(next()%31);
   MEMF(0x10008+12*p)=(int)(next()%23)-11;
  }
  for(unsigned i=0;i<n;++i) {
   unsigned p=next()%40;SMEM16(0x20000+2*i)=p;
   float x=MEMF(0x10000+12*p),z=MEMF(0x10008+12*p);
   if(x<minx||(x==minx&&z>maxz)){expected=i;minx=x;maxz=z;}
  }
  if(run_boundary(boundary_fixed,n)!=expected)return 4;
  old_failures+=run_boundary(boundary_old,n)!=expected;++cases;
 }
 if(!old_failures)return 5;
 printf("%u complete boundary-loop cases pass; old code fails %u\n",cases,old_failures);
 float values[]={-INFINITY,-2,-0.0f,0,0.1f,2,INFINITY,NAN};
 unsigned ai_cases=0,ai_failures=0;
 for(unsigned a=0;a<8;++a)for(unsigned b=0;b<8;++b)for(unsigned c=0;c<8;++c) {
  int expected=!isnan(values[a])&&!isnan(values[b])&&values[a]>values[b];
  if(run_ai(ai_fixed,values[a],values[b],values[c])!=(uint32_t)expected)return 6;
  ai_failures+=run_ai(ai_old,values[a],values[b],values[c])!=(uint32_t)expected;++ai_cases;
 }
 if(!ai_failures)return 7;
 printf("%u AI threshold cases pass; old code fails %u\n",ai_cases,ai_failures);
 return 0;
}
'''.replace('CONSTANTS', '\n'.join(f'MEM32(0x{va:X})=0x{value:X};' for va, value in constants.items()))
        with tempfile.TemporaryDirectory(prefix='merc-sse-address-') as directory:
            path = Path(directory)
            (path / 'test.c').write_text(prelude + boundary + old + ai + ai_old + harness)
            exe = path / 'test.exe'
            subprocess.run([shutil.which('gcc') or 'C:/MinGW/bin/gcc.exe', '-O2', '-fno-strict-aliasing',
                            '-std=c11', str(path / 'test.c'), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    unittest.main()
