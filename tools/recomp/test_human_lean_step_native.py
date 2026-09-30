"""Retail human yaw/lean must step toward targets without snapping/overshoot."""
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
from generated_test_utils import generated_text_containing


class HumanLeanTests(unittest.TestCase):
    def test_executed_lean_block_at_multiple_frame_times(self):
        self.check_step('lean',0x13DFE1,'loc_0013DFCA: ;','loc_0013DFFB: ;')

    def test_executed_yaw_block_at_multiple_frame_times(self):
        self.check_step('yaw',0x13DEBD,'loc_0013DE9B: ;','loc_0013DED7: ;')

    def check_step(self, kind, retail_address, begin, end):
        xbe = ROOT / 'game_files/mercenaries-retail/default.xbe'
        config.configure_from_xbe(str(xbe))
        raw = xbe.read_bytes()
        expected = bytes.fromhex('f30f58c20f2fc1eb0c0f2fc1760cf30f5cc20f2fc876030f28c1')
        offset = config.va_to_file_offset(retail_address)
        self.assertEqual(raw[offset:offset + len(expected)], expected)
        source = generated_text_containing(begin)
        fixed = source[source.index(begin):source.index(end)]
        patches = runpy.run_path(str(ROOT / 'ports/mercenaries/scripts/Patch-Generated.py'))['PATCHES']
        old = fixed
        for p in patches:
            if p.name.startswith('Human '+kind+' '):
                self.assertIn(p.after, fixed)
                old = old.replace(p.after, p.before)
        self.assertNotEqual(old, fixed)
        prefix = r'''
#include <assert.h>
#include <stdint.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#define xmm0 xmm0v[0]
#define xmm1 xmm1v[0]
#define xmm2 xmm2v[0]
#define xmm3 xmm3v[0]
#define xmm4 xmm4v[0]
#define MEMF(a) fixture_read((a),n,rate,dt)
#define recomp_xmm_loadss(v,a) do {(v)[0]=MEMF(a);(v)[1]=(v)[2]=(v)[3]=0;}while(0)
#define recomp_xmm_copy(a,b) memcpy(a,b,sizeof(a))
'''
        field = '0x1028' if kind=='lean' else '0x102C'
        rate_address = '0x30E884' if kind=='lean' else '0x30E870'
        prefix += ('static float fixture_read(uint32_t a,float n,float rate,float dt){'
                   'if(a=='+field+')return n;if(a=='+rate_address+')return rate;'
                   'assert(a==0x2008);return dt;}\n')
        def wrap(name, body):
            return ('static float '+name+'(float n,float t,float rate,float dt){\n'
                    'uint32_t esi=0x1000,ebp=0x2000;int _flags=0;\n'
                    'float xmm0v[4]={'+('n' if kind=='lean' else '-t')+'},'
                    'xmm1v[4]={t},xmm2v[4]={0},xmm3v[4]={0},xmm4v[4]={0};\n'
                    + body + '\n'+end+' return xmm0;\n}\n')
        tail = r'''
static float reference(float n,float t,float rate,float dt) {
 float step=rate*dt;
 if(n<t){n+=step;if(n>t)n=t;}else if(n>t){n-=step;if(n<t)n=t;}return n;
}
int main(void){
 const float dt[]={0,1.0f/120,1.0f/60,1.0f/30,0.05f,0.1f,0.25f,1.0f};
 unsigned count=0,old_failures=0;
 for(int a=-20;a<=20;a++)for(int b=-20;b<=20;b++)for(unsigned i=0;i<8;i++){
   float n=a/10.0f,t=b/10.0f,want=reference(n,t,2,dt[i]);
   float actual=fixed(n,t,2,dt[i]);assert(memcmp(&actual,&want,4)==0);
   old_failures+=old(n,t,2,dt[i])!=want;++count;
 }
 assert(old_failures>0);
 assert(old(-1,1,2,0.1f)==1); /* old code snaps straight to target */
 assert(old(0.9f,1,2,0.1f)>1); /* old code retains overshoot */
 printf("%u smoothing/time-step cases pass; original fails %u; speeds/thresholds unchanged\n",count,old_failures);
 return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='merc-human-lean-') as directory:
            path = Path(directory); c = path/'test.c'; exe = path/'test.exe'
            c.write_text(prefix + wrap('fixed', fixed) + wrap('old', old) + tail)
            subprocess.run([shutil.which('gcc') or 'C:/MinGW/bin/gcc.exe', '-O2', '-std=c11', str(c), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    unittest.main()
