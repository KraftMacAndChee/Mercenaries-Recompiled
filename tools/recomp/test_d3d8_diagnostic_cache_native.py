"""Execute the draw diagnostic cache and preserve live screenshot selection."""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class DeviceDiagnosticCacheTests(unittest.TestCase):
    def test_cached_absent_empty_present_and_live_paths(self):
        source = (ROOT/'src/d3d/d3d8_device.c').read_text(encoding='utf-8')
        cache = re.search(r'static const char \*d3d8_cached_getenv\(const char \*name\)\n\{.*?\n\}', source, re.S)[0]
        for name in ('dev_DrawPrimitive', 'dev_DrawIndexedPrimitive',
                     'dev_DrawPrimitiveUP', 'dev_DrawIndexedPrimitiveUP'):
            body = re.search(r'static HRESULT __stdcall '+name+r'\(.*?\n\}', source, re.S)[0]
            self.assertNotRegex(body, r'\bgetenv\(')
            self.assertIn('d3d8_cached_getenv("MERCENARIES_DEBUG_SOLID_PIXEL")', body)
        for name, option in (
            ('d3d8_WaitForGuestFrameSlot', 'MERCENARIES_DISABLE_FLIP_PACING'),
            ('d3d8_PresentFrame', 'MERCENARIES_CAPTURE_FLIP_PATH'),
        ):
            body = re.search(r'(?:static )?void '+name+r'\(.*?\n\}', source, re.S)[0]
            self.assertNotRegex(body, r'\bgetenv\(')
            self.assertIn(f'd3d8_cached_getenv("{option}")', body)
        stats = source[source.index('static void d3d8_debug_backbuffer_stats(void)\n{'):]
        stats = stats[:stats.index('\n}\n')+3]
        self.assertNotRegex(stats, r'\bgetenv\(')
        selection = stats[stats.index('    flip_capture_path ='):stats.index('    if ((!g_debug_force_capture')]
        prelude = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
static unsigned calls;
static const char *option;
static const char *fake_getenv(const char *name){++calls;return option;}
#define getenv fake_getenv
'''
        harness = prelude+cache+r'''
static int g_debug_capture_at_flip,g_debug_force_capture;
static char g_debug_armed_capture_path[128],g_debug_forced_capture_path[128];
static const char *selected;
static void select_capture(void){
 const char *flip_capture_path,*capture_path;
 selected=NULL;
'''+selection+r'''
 selected=capture_path;
}
int main(int argc,char **argv){
 int mode=atoi(argv[1]);option=mode==0?NULL:mode==1?"":"configured.bmp";
 const char *key="MERCENARIES_DEBUG_SOLID_PIXEL",*expected=option;
 for(unsigned i=0;i<100000;i++)assert(d3d8_cached_getenv(key)==expected);
 assert(calls==1);
 option="changed";assert(d3d8_cached_getenv(key)==expected);assert(calls==1);
 option=expected;
 unsigned cases=0;
 for(unsigned pass=0;pass<100;pass++)
 for(unsigned armed=0;armed<2;armed++)for(unsigned forced=0;forced<2;forced++)
 for(unsigned flip=0;flip<2;flip++)for(unsigned force=0;force<2;force++){
  strcpy(g_debug_armed_capture_path,armed?"live-flip.bmp":"");
  strcpy(g_debug_forced_capture_path,forced?"live-explicit.bmp":"");
  g_debug_capture_at_flip=flip;g_debug_force_capture=force;
  select_capture();
  const char *fp=armed?g_debug_armed_capture_path:expected;
  const char *want=(fp&&!flip&&!force)?NULL:
      force?(forced?g_debug_forced_capture_path:expected):
      (flip&&fp)?fp:expected;
  assert((selected==NULL)==(want==NULL));
  if(want)assert(strcmp(selected,want)==0);
  ++cases;
 }
 /* A configured flip path bypasses splash selection; absent needs all three. */
 assert(calls==(mode==0?4u:3u));
 unsigned slots=64-calls,before=calls;char keys[64][32];
 for(unsigned i=0;i<64;i++){
  snprintf(keys[i],sizeof(keys[i]),"DIAGNOSTIC_%u",i);
  assert(d3d8_cached_getenv(keys[i])==expected);
 }
 assert(calls==before+64);
 before=calls;
 for(unsigned i=0;i<slots;i++)assert(d3d8_cached_getenv(keys[i])==expected);
 assert(calls==before);
 assert(d3d8_cached_getenv(keys[63])==expected);assert(calls==before+1);
 printf("mode=%d: 100000 draw lookups, %u live capture cases and cache overflow pass\n",mode,cases);
}
'''
        with tempfile.TemporaryDirectory(prefix='merc-d3d8-cache-') as directory:
            path=Path(directory); c=path/'test.c'; exe=path/'test.exe'
            c.write_text(harness,encoding='utf-8')
            subprocess.run([shutil.which('gcc') or 'C:/MinGW/bin/gcc.exe',
                            '-O2','-std=c11',str(c),'-o',str(exe)],check=True)
            for mode in range(3):
                subprocess.run([str(exe),str(mode)],check=True)


if __name__=='__main__':unittest.main()
