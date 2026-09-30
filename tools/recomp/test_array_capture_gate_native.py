"""Execute the opt-in array-capture gate; no process access or GPU required."""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class ArrayCaptureGateTests(unittest.TestCase):
    def test_durable_state_contains_all_attributes_textures_and_combiners(self):
        source = (ROOT/'src/nv2a/nv2a_pgraph_d3d11.c').read_text(encoding='utf-8')
        body = re.search(r'static void debug_write_array_capture_state\(FILE \*output\)\n\{.*?\n\}', source, re.S)[0]
        self.assertIn('"%s.state.txt"', source)
        self.assertIn('debug_write_array_capture_state(dump);', source)
        prelude = r'''
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
static struct {
 struct {uint32_t draw_calls;} stats;
 uint32_t host_vsh_hash,draw_mode,combiner_control,shader_stage_program;
 uint32_t shader_other_stage_input,combiner_final_inputs_0,combiner_final_inputs_1;
 struct {uint32_t format;} vertex_array[16];
 uint32_t inline_array_offsets[16];
 struct {int enabled;uint32_t format,image_rect,control0,control1,address,filter;} tex[4];
 uint32_t combiner_color_icw[8],combiner_color_ocw[8];
 uint32_t combiner_alpha_icw[8],combiner_alpha_ocw[8];
} g_pg;
static uint32_t resolved_vertex_array_offset(uint32_t i){return 0x1000+32*i;}
static uint32_t resolved_texture_offset(uint32_t i){return 0x8000+0x100*i;}
'''
        tail = r'''
int main(void) {
 debug_write_array_capture_state(NULL);
 g_pg.stats.draw_calls=41;g_pg.host_vsh_hash=0x414b1e81;
 for(unsigned i=0;i<16;i++)g_pg.vertex_array[i].format=0x2040+i;
 for(unsigned i=0;i<4;i++){g_pg.tex[i].enabled=1;g_pg.tex[i].filter=0x02063f20+i;}
 for(unsigned i=0;i<8;i++)g_pg.combiner_color_icw[i]=0x12340000+i;
 FILE *f=tmpfile();assert(f);debug_write_array_capture_state(f);
 rewind(f);char text[8192];size_t n=fread(text,1,sizeof(text)-1,f);text[n]=0;fclose(f);
 assert(strstr(text,"draw=42 vsh=414B1E81"));
 for(unsigned i=0;i<16;i++) {char expected[128];
  snprintf(expected,sizeof(expected),"slot=%u format=%08X offset=%08X",i,0x2040+i,0x1000+32*i);
  assert(strstr(text,expected));}
 for(unsigned i=0;i<4;i++) {char expected[128];
  snprintf(expected,sizeof(expected),"stage=%u enabled=1 offset=%08X",i,0x8000+0x100*i);
  assert(strstr(text,expected));
  snprintf(expected,sizeof(expected),"filter=%08X",0x02063f20+i);assert(strstr(text,expected));}
 for(unsigned i=0;i<8;i++){char expected[128];
  snprintf(expected,sizeof(expected),"combiner%u color=%08X",i,0x12340000+i);assert(strstr(text,expected));}
 return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='merc-array-state-') as directory:
            path=Path(directory);c=path/'test.c';exe=path/'test.exe'
            c.write_text(prelude+body+tail,encoding='utf-8')
            build=subprocess.run([shutil.which('gcc') or 'C:/MinGW/bin/gcc.exe','-std=c11',str(c),'-o',str(exe)],capture_output=True)
            self.assertEqual(build.returncode,0,build.stderr.decode(errors='replace'))
            run=subprocess.run([str(exe)],capture_output=True)
            self.assertEqual(run.returncode,0,run.stderr.decode(errors='replace'))

    def test_real_gate_and_pair_snapshot(self):
        source = (ROOT/'src/nv2a/nv2a_pgraph_d3d11.c').read_text(encoding='utf-8')
        body = re.search(r'static int debug_array_capture_gate_open\(void\)\n\{.*?\n\}', source, re.S)[0]
        self.assertEqual(source.count('const int array_capture_gate_open = debug_array_capture_gate_open();'), 1)
        self.assertIn('!captured_array_vsh_before && array_capture_gate_open &&', source)
        self.assertIn('!captured_array_vsh && array_capture_gate_open &&', source)
        self.assertEqual(source.count('pgraph_cached_getenv("MERCENARIES_CAPTURE_ARRAY_STRIDE")'),2)
        self.assertEqual(source.count('(!capture_stride_env || stride ==\n                (uint32_t)strtoul(capture_stride_env, NULL, 0)) &&'),2)
        prelude = r'''
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
typedef uint32_t DWORD;
#define INVALID_FILE_ATTRIBUTES 0xffffffffu
#define FILE_ATTRIBUTE_DIRECTORY 0x10u
static const char *configured;
static DWORD attributes=INVALID_FILE_ATTRIBUTES;
static unsigned queries,env_queries;
static const char *pgraph_cached_getenv(const char *name) {
 assert(!strcmp(name,"MERCENARIES_CAPTURE_ARRAY_GATE_FILE"));
 ++env_queries;return configured;
}
static DWORD GetFileAttributesA(const char *path) {
 assert(path==configured);++queries;return attributes;
}
'''
        tail = r'''
int main(int argc,char **argv) {
 int mode=atoi(argv[1]);configured=mode==0?NULL:mode==1?"":"gate";
 if(mode<2) {
  for(int i=0;i<100;i++)assert(debug_array_capture_gate_open()==1);
  assert(queries==0 && env_queries==1);return 0;
 }
 assert(!debug_array_capture_gate_open());
 attributes=FILE_ATTRIBUTE_DIRECTORY;assert(!debug_array_capture_gate_open());
 attributes=0x20;assert(debug_array_capture_gate_open());
 const int same_draw=debug_array_capture_gate_open();
 attributes=INVALID_FILE_ATTRIBUTES;
 assert(same_draw && same_draw); /* before/after share the captured decision */
 assert(!debug_array_capture_gate_open());
 attributes=0;assert(debug_array_capture_gate_open());
 assert(env_queries==1 && queries==6);return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='merc-array-gate-') as directory:
            path=Path(directory);c=path/'test.c';exe=path/'test.exe'
            c.write_text(prelude+body+tail,encoding='utf-8')
            build=subprocess.run([shutil.which('gcc') or 'C:/MinGW/bin/gcc.exe','-std=c11',str(c),'-o',str(exe)],capture_output=True)
            self.assertEqual(build.returncode,0,build.stderr.decode(errors='replace'))
            for mode in range(3):
                run=subprocess.run([str(exe),str(mode)],capture_output=True)
                self.assertEqual(run.returncode,0,run.stderr.decode(errors='replace'))


if __name__=='__main__':unittest.main()
