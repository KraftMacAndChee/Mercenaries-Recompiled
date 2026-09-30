"""Execute the actual helicopter entry guard against retail COMISS semantics.

This checks entry/early-return and ABI, not the subsequent flight integrator.
"""
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
from generated_test_utils import generated_text_containing


class HelicopterDtTests(unittest.TestCase):
    def test_retail_guard_and_native_stack_dependencies(self):
        xbe = ROOT / 'game_files/mercenaries-retail/default.xbe'
        config.configure_from_xbe(str(xbe))
        raw = xbe.read_bytes()
        expected = bytes.fromhex('0f57c083ec240f2f442428568bf10f8310020000')
        offset = config.va_to_file_offset(0x13ADD0)
        self.assertEqual(raw[offset:offset + len(expected)], expected)
        offset = config.va_to_file_offset(0x13AFF4)
        self.assertEqual(raw[offset:offset + 7], bytes.fromhex('5e83c424c20400'))
        text = generated_text_containing('loc_0013ADD0: ;')
        fixed = text[text.index('loc_0013ADD0: ;'):text.index('loc_0013ADE4: ;')]
        epilogue = re.search(r'loc_0013AFF4: ;\n.*?return; /\* ret 4 \*/', text, re.S)[0]
        patch = next(p for p in runpy.run_path(str(ROOT / 'ports/mercenaries/scripts/Patch-Generated.py'))['PATCHES']
                     if p.name == 'Helicopter physics retains delta-time comparison before PUSH')
        self.assertIn(patch.after, fixed)
        old = fixed.replace(patch.after, patch.before)
        self.assertEqual(old.replace(patch.before, patch.after), fixed)
        prelude = r'''
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
static unsigned char mem[0x4000];
static uint32_t esp,esi,ecx,entered;
static float xmm0v[4];
#define xmm0 xmm0v[0]
#define MEM32(a) (*(uint32_t*)(mem+(uint32_t)(a)))
#define MEMF(a) (*(float*)(mem+(uint32_t)(a)))
#define PUSH32(s,v) do{uint32_t t=(v);(s)-=4;MEM32(s)=t;}while(0)
#define POP32(s,v) do{(v)=MEM32(s);(s)+=4;}while(0)
#define recomp_xmm_zero(v) memset((v),0,sizeof(v))
'''
        def function(name, body):
            return ('static void ' + name + '(void) { int _flags=0;\n' + body
                    + '\n++entered;\n' + epilogue + '\n}\n')
        harness = r'''
static unsigned run(void(*fn)(void),float dt,uint32_t ret) {
 memset(mem,0xA5,sizeof(mem));esp=0x2000;esi=0x11223344;ecx=0x1000;entered=0;
 MEM32(esp)=ret;MEMF(esp+4)=dt;fn();
 if(esp!=0x2008||esi!=0x11223344||MEM32(0x2000)!=ret)exit(2);
 return entered;
}
int main(void) {
 unsigned tested=0,failures=0;
 const float times[]={0.0f,-0.0f,0.001f,1.0f/60.0f,1.0f/30.0f,0.1f,1.0f,-0.001f,-1.0f,INFINITY,-INFINITY,NAN};
 const uint32_t returns[]={0,0x13bb20,0x13adef,0x80000000,0x3f800000,0xbf800000,0x7fc00000};
 for(unsigned i=0;i<sizeof(times)/sizeof(times[0]);++i)
 for(unsigned j=0;j<sizeof(returns)/sizeof(returns[0]);++j) {
  unsigned expected=isnan(times[i])||times[i]>0.0f;
  if(run(fixed,times[i],returns[j])!=expected)return 3;
  failures+=run(old,times[i],returns[j])!=expected;++tested;
 }
 if(!failures)return 4;
 printf("%u helicopter dt/return-address cases passed; old code fails %u\n",tested,failures);
 return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='merc-helicopter-dt-') as directory:
            path = Path(directory)
            (path / 'test.c').write_text(prelude + function('fixed', fixed) + function('old', old) + harness)
            exe = path / 'test.exe'
            subprocess.run([shutil.which('gcc') or 'C:/MinGW/bin/gcc.exe', '-O2', '-fno-strict-aliasing',
                            '-std=c11', str(path / 'test.c'), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    unittest.main()
