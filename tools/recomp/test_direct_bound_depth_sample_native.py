"""Native guard for the non-AA active-depth ABGR snapshot route."""
from pathlib import Path
import subprocess,tempfile
ROOT=Path(__file__).resolve().parents[2]
s=(ROOT/'src/nv2a/nv2a_pgraph_d3d11.c').read_text()
start=s.index('static GuestDepthSurface *direct_bound_depth_sample_source(')
body=s[start:s.index('\n/* Title compatibility: xboxSky.vsh',start)]
harness=r'''
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "nv2a_regs.h"
typedef struct { unsigned valid,format,anti_aliasing,offset,logical_width,logical_height,pitch; uint64_t write_serial; } GuestDepthSurface;
typedef struct {unsigned offset;} GuestColorSurface;
static struct { GuestDepthSurface *bound_depth; GuestColorSurface *bound_color; unsigned depth_test,depth_write; struct {unsigned enabled,format,control0,control1,image_rect,offset;} tex[4]; } g_pg;
static unsigned resolved_texture_offset(unsigned stage){return g_pg.tex[stage].offset;}
static unsigned get_guest_texture_levels(unsigned f,unsigned c,unsigned t){return 1;}
static void get_guest_texture_dimensions(unsigned f,unsigned r,unsigned c,unsigned *w,unsigned *h){*w=r>>16;*h=r&65535;}
''' + body + r'''
static GuestDepthSurface depth;static GuestColorSurface color;
static void setup(void){
 memset(&g_pg,0,sizeof(g_pg)); memset(&depth,0,sizeof(depth));
 depth.valid=1;depth.format=NV097_SET_SURFACE_FORMAT_ZETA_Z24S8;
 depth.offset=0x026b4000;depth.logical_width=640;depth.logical_height=480;depth.pitch=2560;depth.write_serial=51;
 color.offset=0x01e34000;g_pg.bound_color=&color;g_pg.bound_depth=&depth;g_pg.depth_test=1;
 g_pg.tex[1].enabled=1;g_pg.tex[1].offset=depth.offset;
 g_pg.tex[1].format=NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8B8G8R8<<8;
 g_pg.tex[1].image_rect=(640<<16)|480;g_pg.tex[1].control1=2560<<16;
}
int main(void){
 setup();assert(direct_bound_depth_sample_source(1)==&depth);
 /* Source remains current even if an independent old color cache exists.
    The returned depth identity must not alter depth or current render targets. */
 GuestDepthSurface original=depth;assert(direct_bound_depth_sample_source(1)==&depth);assert(!memcmp(&original,&depth,sizeof(depth)));
 setup();depth.anti_aliasing=1;assert(!direct_bound_depth_sample_source(1));
 setup();g_pg.tex[1].format=NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8R8G8B8<<8;assert(!direct_bound_depth_sample_source(1));
 setup();g_pg.tex[1].offset+=4096;assert(!direct_bound_depth_sample_source(1));
 setup();color.offset=depth.offset;assert(!direct_bound_depth_sample_source(1));
 setup();g_pg.depth_write=1;assert(!direct_bound_depth_sample_source(1));
 setup();g_pg.depth_test=0;assert(!direct_bound_depth_sample_source(1));
 setup();g_pg.tex[1].enabled=0;assert(!direct_bound_depth_sample_source(1));
 setup();g_pg.tex[1].format|=NV097_SET_TEXTURE_FORMAT_CUBEMAP_ENABLE;assert(!direct_bound_depth_sample_source(1));
 setup();g_pg.tex[1].image_rect=(320<<16)|240;assert(!direct_bound_depth_sample_source(1));
 setup();g_pg.tex[1].control1=5120<<16;assert(!direct_bound_depth_sample_source(1));
 setup();g_pg.bound_depth=NULL;assert(!direct_bound_depth_sample_source(1));
 setup();depth.valid=0;assert(!direct_bound_depth_sample_source(1));
 setup();depth.write_serial=0;assert(!direct_bound_depth_sample_source(1));
 setup();assert(!direct_bound_depth_sample_source(0));
 puts("PASS: live non-AA layout matches; AA, satellite ARGB, stale addresses, writable depth, disabled and incompatible views excluded");
}
'''
with tempfile.TemporaryDirectory(prefix='mercs-direct-depth-') as tmp:
 p=Path(tmp);(p/'test.c').write_text(harness)
 subprocess.run(['C:/MinGW/bin/gcc.exe','-std=c11','-O2','-I',str(ROOT/'src/nv2a'),str(p/'test.c'),'-o',str(p/'test.exe')],check=True)
 subprocess.run([str(p/'test.exe')],check=True)
# This route must not populate the persistent color/depth cache or modify its serials.
assert '->write_serial =' not in body and 'gpu_drawn =' not in body
array=s[s.index('static void submit_array_draw(void)'):s.index('static void submit_draw(void)')]
assert 'feedback_srvs[i] = prepare_satellite_depth_sample(direct_depth)' in array
assert 'ID3D11ShaderResourceView_Release(feedback_srvs[i])' in array
print('PASS: snapshot is draw-local and released; persistent alias ownership unchanged')
