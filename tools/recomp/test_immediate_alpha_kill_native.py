"""Execute production immediate-draw alpha-kill state synchronization."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class ImmediateAlphaKillTests(unittest.TestCase):
    def test_all_previous_and_current_stage_masks(self):
        source = (ROOT/'src/nv2a/nv2a_pgraph_d3d11.c').read_text(encoding='utf-8')
        start = source.index('            dev->lpVtbl->SetTextureStageState(',
                             source.index('/* Immediate draws share the host sampler state'))
        end = source.index('\n\n', start)
        body = source[start:end]
        prelude = r'''
#include <assert.h>
#include <stdint.h>
#define D3DTSS_ALPHAKILL 100
#define NV_PGRAPH_TEXCTL0_0_ALPHAKILLEN (1u<<2)
typedef struct Device Device;
typedef struct { void (*SetTextureStageState)(Device*,unsigned,unsigned,unsigned); } Vtbl;
struct Device { const Vtbl *lpVtbl; };
static struct { struct { uint32_t control0; } tex[4]; } g_pg;
static unsigned host[4], calls;
static void set(Device*d,unsigned stage,unsigned state,unsigned value) {
 assert(d && stage<4 && state==D3DTSS_ALPHAKILL && value<=1);
 host[stage]=value; ++calls;
}
static const Vtbl vtbl={set};
static Device device={&vtbl};
static void sync(void) {
 Device *dev=&device;
 for(unsigned stage=0;stage<4;++stage) {
'''
        tail = r'''
 }
}
int main(void) {
 for(unsigned previous=0;previous<16;++previous)
 for(unsigned current=0;current<16;++current)
 for(unsigned unrelated=0;unrelated<16;++unrelated) {
  for(unsigned stage=0;stage<4;++stage) {
   host[stage]=(previous>>stage)&1;
   g_pg.tex[stage].control0=(((current>>stage)&1)<<2)
                         | ((unrelated&1)?0x40000000u:0)
                         | ((unrelated&2)?0x80000000u:0)
                         | ((unrelated&4)?0x00004000u:0)
                         | ((unrelated&8)?0x00000003u:0);
  }
  calls=0; sync(); assert(calls==4);
  for(unsigned stage=0;stage<4;++stage) assert(host[stage]==((current>>stage)&1));
 }
 /* Reproduce the cutout -> occluded-flare transition. Zero must overwrite
    the old visibility value, not be discarded by the previous material. */
 host[0]=1; g_pg.tex[0].control0=0x40000000u;
 sync(); assert(host[0]==0);
 unsigned visibility=128, sampled_alpha=0;
 if(!(host[0] && sampled_alpha==0)) visibility=sampled_alpha;
 assert(visibility==0);
 return 0;
}
'''
        compiler = shutil.which('gcc') or 'C:/MinGW/bin/gcc.exe'
        with tempfile.TemporaryDirectory(prefix='mercs-alpha-kill-') as directory:
            c, exe = Path(directory)/'test.c', Path(directory)/'test.exe'
            for actual, succeeds in ((body,True),('',False)):
                c.write_text(prelude+actual+tail,encoding='utf-8')
                result = subprocess.run([compiler,'-std=c11',str(c),'-o',str(exe)],capture_output=True)
                self.assertEqual(result.returncode,0,result.stderr.decode(errors='replace'))
                result = subprocess.run([str(exe)],capture_output=True)
                self.assertEqual(result.returncode==0,succeeds)


if __name__ == '__main__':
    unittest.main()
