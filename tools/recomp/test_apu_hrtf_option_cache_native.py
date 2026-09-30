"""Execute cached HRTF policy selection before any audio worker starts."""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[2]


class HrtfOptionCacheTests(unittest.TestCase):
    def test_cached_before_workers_and_preserves_all_selection_cases(self):
        source=(ROOT/'src/apu/apu_vp.c').read_text()
        start=source.index('static void mcpx_apu_vp_init_diagnostic_options(')
        init=source[start:source.index('\n}\n',start)+3]
        gate=re.search(r'if \((v < MCPX_HW_MAX_3D_VOICES && d->vp.hrtf_enabled)\)',source)[1]
        self.assertNotIn('getenv',gate)
        vp_init=source[source.index('void mcpx_apu_vp_init('):source.index('void mcpx_apu_vp_finalize(')]
        self.assertLess(vp_init.index('mcpx_apu_vp_init_diagnostic_options(d)'),vp_init.index('MERCENARIES_DISABLE_APU_WORKERS'))
        self.assertLess(vp_init.index('mcpx_apu_vp_init_diagnostic_options(d)'),vp_init.index('qemu_thread_create'))
        self.assertNotIn('hrtf_enabled',source[source.index('void mcpx_apu_vp_reset('):])
        harness=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef struct {struct {bool hrtf_enabled;} vp;} MCPXAPUState;
static struct {struct {bool hrtf;} audio;} g_config;
static const char *enable_option;
static const char *disable_option;
static unsigned lookups;
static const char *fake_getenv(const char *name) {
 ++lookups;
 if(strcmp(name,"MERCENARIES_ENABLE_APU_HRTF")==0)return enable_option;
 assert(strcmp(name,"MERCENARIES_TEST_DISABLE_HRTF")==0);return disable_option;
}
#define getenv fake_getenv
#define MCPX_HW_MAX_3D_VOICES 64
'''+init+r'''
static bool selected(MCPXAPUState *d,unsigned v){return ('''+gate+r''');}
int main(void){
 const char *values[]={NULL,"","0","1"}; unsigned cases=0;
 for(unsigned e=0;e<4;e++)for(unsigned x=0;x<4;x++)for(unsigned configured=0;configured<2;configured++){
  MCPXAPUState d={0};enable_option=values[e];disable_option=values[x];lookups=0;g_config.audio.hrtf=configured;
  mcpx_apu_vp_init_diagnostic_options(&d);assert(lookups==2);
  for(unsigned i=0;i<100000;i++){
   unsigned v=i%256;assert(selected(&d,v)==(v<64&&(configured||values[e]!=NULL)&&values[x]==NULL));++cases;
  }
  assert(lookups==2);
 }
 printf("%u HRTF gate cases pass; two option lookups per initialization, zero per voice\n",cases);
 return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='merc-hrtf-options-') as directory:
            p=Path(directory); c=p/'test.c'; exe=p/'test.exe'
            c.write_text(harness)
            subprocess.run([shutil.which('gcc') or 'C:/MinGW/bin/gcc.exe','-O2','-std=c11',str(c),'-o',str(exe)],check=True)
            subprocess.run([str(exe)],check=True)


if __name__=='__main__':unittest.main()
