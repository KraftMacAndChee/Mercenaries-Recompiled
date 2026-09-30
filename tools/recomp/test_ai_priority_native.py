"""Exercise the actual lifted AI priority CFG against the source/retail bounds."""
from pathlib import Path
import runpy
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from tools.recomp import config


class AiPriorityTests(unittest.TestCase):
    def test_retail_comparisons_and_native_predecessors(self):
        xbe = ROOT / 'game_files/mercenaries-retail/default.xbe'
        config.configure_from_xbe(str(xbe))
        raw = xbe.read_bytes()
        for address, expected in ((0x6C49C, '0f2fc8'), (0x6C4D4, '0f2fc1'),
                                  (0x6C4DF, '7608')):
            expected = bytes.fromhex(expected)
            offset = config.va_to_file_offset(address)
            self.assertEqual(raw[offset:offset + len(expected)], expected)
        text = (ROOT / 'ports/mercenaries/src/recomp/gen/recomp_0002.c').read_text()
        fixed = text[text.index('loc_0006C465: ;'):text.index('loc_0006C4E9: ;')]
        patches = [p for p in runpy.run_path(str(ROOT / 'ports/mercenaries/scripts/Patch-Generated.py'))['PATCHES']
                   if p.name.startswith('AI priority ')]
        self.assertEqual(len(patches), 3)
        old = fixed
        for p in patches:
            self.assertIn(p.after, old)
            old = old.replace(p.after, p.before)
        rebuilt = old
        for p in patches:
            rebuilt = rebuilt.replace(p.before, p.after)
        self.assertEqual(rebuilt, fixed)
        prelude = r'''
#include <stdint.h>
#include <stdlib.h>
#include <math.h>
#include <stdio.h>
static float mem[0x300000/4];
#define MEMF(a) mem[(uint32_t)(a)/4]
#define xmm0 xmm0v[0]
#define xmm1 xmm1v[0]
#define recomp_xmm_loadss(v,a) ((v)[0]=MEMF(a))
'''
        def function(name, body):
            return ('static float ' + name + '(float base,float current,float dt) {\n'
                    'uint32_t esi=0x1000,esp=0x2000;int _flags=0;float xmm0v[4]={0},xmm1v[4]={0};\n'
                    'MEMF(esi+0x560)=base;MEMF(esi+0x564)=current;MEMF(esp+8)=dt;\n'
                    'MEMF(0x2E42A8)=0.02f;MEMF(0x2DC340)=0.5f;MEMF(0x2DC090)=2.0f;\n'
                    + body + '\nloc_0006C4E9: return MEMF(esi+0x564);\n}\n')
        harness = r'''
int main(void) {
 unsigned tested=0,old_failures=0;
 for(int b=0;b<9;++b)for(int c=0;c<41;++c)for(int d=0;d<5;++d) {
  float base=b*0.125f,current=c*0.0625f,dt=(float[]){0,0.016f,0.1f,1,20}[d];
  float expected=current;
  if(current<base) {expected+=dt*0.02f;if(expected<0.5f*base)expected=0.5f*base;}
  else if(current>base) {expected-=dt*0.02f;if(expected>2*base)expected=2*base;}
  if(fabsf(fixed(base,current,dt)-expected)>1e-6f)return 2;
  old_failures+=fabsf(old(base,current,dt)-expected)>1e-6f;++tested;
 }
 if(old_failures==0)return 3;
 if(!isnan(fixed(0.5f,NAN,0.1f)))return 4;
 if(fixed(NAN,0.25f,0.1f)!=0.25f)return 5;
 printf("%u priority cases passed; old code fails %u cases\n",tested,old_failures);
 return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='merc-ai-priority-') as directory:
            path = Path(directory)
            (path / 'test.c').write_text(prelude + function('fixed', fixed) + function('old', old) + harness)
            exe = path / 'test.exe'
            subprocess.run([shutil.which('gcc') or 'C:/MinGW/bin/gcc.exe', '-O2', '-std=c11',
                            str(path / 'test.c'), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    unittest.main()
