"""Exercise production opt-in flare-pass capture guards in a native harness."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class FlareProbeCaptureTests(unittest.TestCase):
    def test_disabled_minimum_resource_and_count_guards(self):
        text = (ROOT / 'src/nv2a/nv2a_pgraph_d3d11.c').read_text(encoding='utf-8')
        start = text.index('    if (use_flare_probe_vsh) {\n        static uint32_t captured_flare_probe_passes;')
        end = text.index('\n    }\n', start) + len('\n    }')
        code = text[start:end]
        prelude = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define MAX_PATH 260
#define CHECK(c) do {if(!(c))exit(1);}while(0)
typedef struct {void *texture; uint32_t offset,width,height;} Surface;
static struct {
 struct {uint32_t draw_calls;} stats;
 Surface *bound_color;
 uint32_t surface_zeta_offset,depth_test,depth_write,depth_func,color_mask;
 uint32_t cull_enable,cull_face,front_face,combiner_factor0[1],combiner_factor1[1];
 struct {uint32_t format,image_rect,control1;} tex[1];
 float immediate_vertices[4][16][4];
} g_pg;
static const char *prefix;
static const char *gate;
static int gate_exists;
static unsigned captures;
static unsigned depth_captures;
#define INVALID_FILE_ATTRIBUTES 0xffffffffu
static uint32_t GetFileAttributesA(const char *path) {
 CHECK(path==gate);return gate_exists ? 0x20u : INVALID_FILE_ATTRIBUTES;
}
static const char *pgraph_cached_getenv(const char *name) {
 if(!strcmp(name,"MERCENARIES_CAPTURE_FLARE_PROBE_PREFIX"))return prefix;
 if(!strcmp(name,"MERCENARIES_CAPTURE_DRAW_MIN"))return "1000";
 if(!strcmp(name,"MERCENARIES_CAPTURE_FLARE_PROBE_GATE_FILE"))return gate;
 return NULL;
}
static void debug_trace_flare_probe_depth(uint32_t index,uint32_t vertices) {
 CHECK(vertices==4 && index==depth_captures && captures==index+1);++depth_captures;
}
static uint32_t resolved_texture_offset(unsigned i) {CHECK(i==0);return 0x20000;}
static int combiner_state;
static void debug_trace_flare_sample_source(const char *value, uint32_t index, const void *state) {
 CHECK(state==&combiner_state);
 CHECK(value==prefix && index+1==captures);
}
static void d3d8_DebugCaptureTextureToPath(void *texture,const char *path) {
 char expected[260];CHECK(texture==(void*)1);
 snprintf(expected,sizeof(expected),"probe-%02u-00010000.bmp",captures);
 CHECK(!strcmp(path,expected));++captures;
}
static void capture(int use_flare_probe_vsh) {
 unsigned num_verts=4, immediate_draw_shader=1, use_immediate_combiners=1;
'''
        harness = r'''
}
int main(void) {
 Surface surface={(void*)1,0x10000,640,480};
 g_pg.bound_color=&surface;g_pg.stats.draw_calls=1000;
 capture(1);CHECK(captures==0);
 prefix="";capture(1);CHECK(captures==0);
 prefix="probe";capture(0);CHECK(captures==0);
 g_pg.stats.draw_calls=999;capture(1);CHECK(captures==0);
 g_pg.stats.draw_calls=1000;g_pg.bound_color=NULL;capture(1);CHECK(captures==0);
 g_pg.bound_color=&surface;surface.texture=NULL;capture(1);CHECK(captures==0);
 surface.texture=(void*)1;
 gate="capture.flag";
 for(unsigned i=0;i<50;++i)capture(1);
 CHECK(captures==0 && depth_captures==0);
 gate_exists=1;
 for(unsigned i=0;i<150;++i)capture(1);
 CHECK(captures==128 && depth_captures==128);return 0;
}
'''
        compiler = shutil.which('gcc') or 'C:/MinGW/bin/gcc.exe'
        with tempfile.TemporaryDirectory(prefix='mercs-flare-capture-') as directory:
            c = Path(directory) / 'test.c'
            exe = Path(directory) / 'test.exe'
            c.write_text(prelude + code + harness, encoding='utf-8')
            result = subprocess.run([compiler, '-std=c11', str(c), '-o', str(exe)], capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr.decode(errors='replace'))
            result = subprocess.run([str(exe)], capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr.decode(errors='replace'))


if __name__ == '__main__':
    unittest.main()
