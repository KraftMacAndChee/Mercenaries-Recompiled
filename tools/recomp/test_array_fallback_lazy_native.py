"""Compile the actual lazy fallback helpers and compare legacy output bytes."""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[2]


class LazyArrayFallbackTests(unittest.TestCase):
    def test_native_output_equivalence_and_programmable_no_work(self):
        source=(ROOT/'src/nv2a/nv2a_pgraph_d3d11.c').read_text(encoding='utf-8')
        names=['decode_array_fallback_vertex','prepare_array_fallback_vertices']
        bodies=[re.search(r'static OutputVertex(?: \*| )'+name+r'\(.*?\n\}',source,re.S)[0] for name in names]
        draw=source[source.index('static void submit_array_draw('):source.index('static void mirror_guest_fixed_function_blit(')]
        self.assertNotIn('out[i]',draw)
        self.assertLess(draw.index('draw_shader = prepare_transform_program('),draw.index('out = prepare_array_fallback_vertices('))
        self.assertIn('if (!draw_shader && !out)',draw)
        prelude=r'''
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
typedef uint32_t DWORD;
typedef struct {float x,y,z,rhw;uint32_t color;float u,v;} OutputVertex;
static unsigned allocations,reads;static int fail_allocation;
static void *counted_malloc(size_t n){++allocations;return fail_allocation?NULL:malloc(n);}
#define malloc counted_malloc
static uint32_t read_le32(const uint8_t *p){uint32_t v;memcpy(&v,p,4);++reads;return v;}
static float u2f(uint32_t v){float f;memcpy(&f,&v,4);return f;}
static OutputVertex legacy(const uint8_t *src,uint32_t stride,int linear_uv,uint32_t tex_width,uint32_t tex_height){
 OutputVertex out;
 out.x = stride >= 4u ? u2f(read_le32(src + 0u)) : 0.0f;
 out.y = stride >= 8u ? u2f(read_le32(src + 4u)) : 0.0f;
 out.z = stride >= 12u ? u2f(read_le32(src + 8u)) : 0.0f;
 out.rhw = stride >= 16u ? u2f(read_le32(src + 12u)) : 1.0f;
 out.color = 0xFFFFFFFFu;
 out.u = stride >= 20u ? u2f(read_le32(src + 16u)) : 0.0f;
 out.v = stride >= 24u ? u2f(read_le32(src + 20u)) : 0.0f;
 if(linear_uv && tex_width && tex_height){out.u/=(float)tex_width;out.v/=(float)tex_height;}
 return out;
}
'''
        tail=r'''
int main(void){
 unsigned cases=0;uint32_t random=0x84903172;uint8_t data[256];
 assert(prepare_array_fallback_vertices(1,NULL,0xffffffffu,0xffffffffu,1,1,1)==NULL);
 assert(allocations==0 && reads==0);
 assert(prepare_array_fallback_vertices(0,NULL,24,0,1,1,1)==NULL);
 assert(allocations==0 && reads==0);
 for(unsigned pattern=0;pattern<16;pattern++){
  for(unsigned i=0;i<sizeof(data);i++){random=random*1664525u+1013904223u;data[i]=(uint8_t)(random>>24);}
  for(unsigned stride=0;stride<=64;stride++)for(unsigned linear=0;linear<2;linear++)
   for(unsigned dims=0;dims<4;dims++){
    unsigned w=(dims&1)?640:0,h=(dims&2)?480:0;
    OutputVertex *actual=prepare_array_fallback_vertices(0,data,stride,3,linear,w,h);assert(actual);
    for(unsigned i=0;i<3;i++){OutputVertex expected=legacy(data+i*stride,stride,linear,w,h);
     assert(!memcmp(&expected,&actual[i],sizeof(expected)));++cases;}
    free(actual);
   }
 }
 unsigned before_reads=reads;fail_allocation=1;
 assert(prepare_array_fallback_vertices(0,NULL,24,1,1,1,1)==NULL);
 assert(reads==before_reads);
 unsigned before_allocations=allocations;
 for(unsigned i=0;i<10000;i++)assert(!prepare_array_fallback_vertices(1,NULL,24,100000,1,640,480));
 assert(reads==before_reads && allocations==before_allocations);
 printf("%u byte-identical fallback vertices; 10001 programmable calls allocate/read nothing\n",cases);
 return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='merc-lazy-array-') as directory:
            path=Path(directory);c=path/'test.c';exe=path/'test.exe'
            c.write_text(prelude+'\n'.join(bodies)+tail,encoding='utf-8')
            build=subprocess.run([shutil.which('gcc') or 'C:/MinGW/bin/gcc.exe','-O2','-std=c11',str(c),'-o',str(exe)],capture_output=True)
            self.assertEqual(build.returncode,0,build.stderr.decode(errors='replace'))
            run=subprocess.run([str(exe)],capture_output=True)
            self.assertEqual(run.returncode,0,run.stderr.decode(errors='replace'))
            print(run.stdout.decode().strip())


if __name__=='__main__':unittest.main()
