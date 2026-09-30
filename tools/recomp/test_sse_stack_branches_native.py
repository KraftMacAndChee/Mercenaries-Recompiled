"""Retail COMISS operands must be evaluated before flag-neutral stack changes."""
from pathlib import Path
import re
import runpy
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from tools.recomp import config


class SseStackBranchTests(unittest.TestCase):
    def test_retail_blocks_against_independent_stack_slots(self):
        repairs = runpy.run_path(str(ROOT / 'ports/mercenaries/scripts/Patch-Generated.py'))['SSE_STACK_COMPARE_REPAIRS']
        config.configure_from_xbe(str(ROOT / 'game_files/mercenaries-retail/default.xbe'))
        raw = (ROOT / 'game_files/mercenaries-retail/default.xbe').read_bytes()
        generated = '\n'.join(p.read_text() for p in (ROOT / 'ports/mercenaries/src/recomp/gen').glob('recomp_*.c'))
        prelude = r'''
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
static unsigned char mem[0x4000];
static uint32_t esp,esi,ebp,ebx,ecx;
static float xmm0;
#define MEM32(a) (*(uint32_t*)(mem+(uint32_t)(a)))
#define MEMF(a) (*(float*)(mem+(uint32_t)(a)))
#define PUSH32(s,v) do{uint32_t t=(v);(s)-=4;MEM32(s)=t;}while(0)
#define POP32(s,v) do{(v)=MEM32(s);(s)+=4;}while(0)
'''
        functions = []
        metadata = []
        for site, va, encoded, delta, before, after in repairs:
            offset = config.va_to_file_offset(va)
            expected = bytes.fromhex(encoded)
            self.assertEqual(raw[offset:offset + len(expected)], expected)
            self.assertEqual(generated.count(after), 1, site)
            self.assertNotIn(before, generated)
            slot = int(re.search(r'MEMF\(esp \+ (0x[0-9A-F]+)\)', before)[1], 16)
            target = re.search(r'goto (loc_[0-9A-F]+)', before)[1]
            comparison_jae = '/* jae:' in before
            metadata.append('{fixed_' + site + ',old_' + site + f',{slot},{delta},{int(comparison_jae)}' + '}')
            for name, body in [('fixed_', after), ('old_', before)]:
                # Actual producer, neutral instructions and branch only. The
                # harness supplies the pre-COMISS state, not preceding logic.
                block = body[body.index('    /* comiss'):]
                functions.append('static int ' + name + site + '(void) {int _flags=0;\n' + block
                                 + '\nreturn 0;\n' + target + ': return 1;\n}\n')
        harness = r'''
static int run(int(*fn)(void),int slot,int delta,float lhs,float rhs,float wrong) {
 memset(mem,0xA5,sizeof(mem));esp=0x2000;esi=11;ebp=12;ebx=13;ecx=14;xmm0=lhs;
 MEM32(esp)=21;MEM32(esp+4)=22;MEM32(esp+8)=23;
 MEMF(esp+slot)=rhs;MEMF(esp+slot+delta)=wrong;
 int result=fn();
 if(esp!=(uint32_t)(0x2000+delta))exit(2);
 if(delta<0) {if(esi!=14||MEM32(esp)!=11||ebp!=12||ebx!=13)exit(3);}
 if(delta==4&&ebp!=21)exit(4);
 if(delta==8&&(esi!=21||ebx!=22))exit(5);
 if(delta==12&&(esi!=21||ebp!=22||ebx!=23))exit(6);
 return result;
}
struct test {int(*fixed)(void),(*old)(void);int slot,delta,jae;};
int main(void) {
 struct test tests[]= {METADATA};
 float values[]={-INFINITY,-2,-0.0f,0,0.033333f,2,INFINITY,NAN};
 unsigned total=0,old_failures=0;
 for(unsigned t=0;t<sizeof(tests)/sizeof(tests[0]);++t) {
  unsigned failures=0;
  for(unsigned a=0;a<8;++a)for(unsigned b=0;b<8;++b)for(unsigned c=0;c<8;++c) {
   struct test *p=&tests[t];float lhs=values[a],rhs=values[b],wrong=values[c];
   int unordered=isnan(lhs)||isnan(rhs);
   int expected=p->jae ? (!unordered&&lhs>=rhs) : (unordered||lhs<=rhs);
   if(run(p->fixed,p->slot,p->delta,lhs,rhs,wrong)!=expected)return 7;
   failures+=run(p->old,p->slot,p->delta,lhs,rhs,wrong)!=expected;++total;
  }
  if(!failures)return 8;old_failures+=failures;
 }
 printf("%u SSE stack-slot cases pass; old code fails %u across all five sites\n",total,old_failures);
 return 0;
}
'''.replace('METADATA', ','.join(metadata))
        with tempfile.TemporaryDirectory(prefix='merc-sse-stack-') as directory:
            path = Path(directory)
            (path / 'test.c').write_text(prelude + '\n'.join(functions) + harness)
            exe = path / 'test.exe'
            subprocess.run([shutil.which('gcc') or 'C:/MinGW/bin/gcc.exe', '-O2', '-fno-strict-aliasing',
                            '-std=c11', str(path / 'test.c'), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    unittest.main()
