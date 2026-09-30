"""One-shot diagnostic capture arming does not change gameplay state."""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class DrawSeriesPhaseGateTests(unittest.TestCase):
    def test_real_gate(self):
        source = (ROOT/'src/nv2a/nv2a_pgraph_d3d11.c').read_text(encoding='utf-8')
        body = re.search(r'static int debug_draw_series_phase_active\(void\)\n\{.*?\n\}', source, re.S)[0]
        self.assertEqual(source.count('debug_draw_series_phase_active() &&'), 1)
        prelude = r'''
#include <stdint.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
typedef uint32_t DWORD;
#define INVALID_FILE_ATTRIBUTES 0xffffffffu
#define FILE_ATTRIBUTE_DIRECTORY 0x10u
static uint32_t g_mercenaries_gameplay_capture_active;
static const char *configured;
static unsigned env_queries,required_queries,queries;
static DWORD attributes=INVALID_FILE_ATTRIBUTES;
static const char *pgraph_cached_getenv(const char *name){
 if(!strcmp(name,"MERCENARIES_CAPTURE_DRAW_GATE_REQUIRED")){
  ++required_queries;return NULL;
 }
 assert(!strcmp(name,"MERCENARIES_CAPTURE_DRAW_GATE_FILE"));++env_queries;return configured;
}
static DWORD GetFileAttributesA(const char *path){assert(path==configured);++queries;return attributes;}
'''
        tail = r'''
int main(int argc,char **argv){
 int mode=atoi(argv[1]);configured=mode==0?NULL:mode==1?"":"gate";
 g_mercenaries_gameplay_capture_active=1;
 assert(debug_draw_series_phase_active());assert(!env_queries && !queries);
 g_mercenaries_gameplay_capture_active=0;
 if(mode<2){
  for(int i=0;i<100;i++)assert(!debug_draw_series_phase_active());
  assert(required_queries==1 && env_queries==1 && queries==0);return 0;
 }
 assert(!debug_draw_series_phase_active());
 attributes=FILE_ATTRIBUTE_DIRECTORY;assert(!debug_draw_series_phase_active());
 attributes=0x20;assert(debug_draw_series_phase_active());
 attributes=INVALID_FILE_ATTRIBUTES;
 for(int i=0;i<100;i++)assert(debug_draw_series_phase_active());
 assert(required_queries==1 && env_queries==1 && queries==3);
 assert(g_mercenaries_gameplay_capture_active==0);
 return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='merc-draw-phase-') as directory:
            path=Path(directory); c=path/'test.c'; exe=path/'test.exe'
            c.write_text(prelude+body+tail,encoding='utf-8')
            build=subprocess.run([shutil.which('gcc') or 'C:/MinGW/bin/gcc.exe','-std=c11',str(c),'-o',str(exe)],capture_output=True)
            self.assertEqual(build.returncode,0,build.stderr.decode(errors='replace'))
            for mode in range(3):
                run=subprocess.run([str(exe),str(mode)],capture_output=True)
                self.assertEqual(run.returncode,0,run.stderr.decode(errors='replace'))


if __name__=='__main__':unittest.main()
