"""Exercise the production sky-only screen-depth eligibility guard."""
from pathlib import Path
import subprocess, tempfile
ROOT=Path(__file__).resolve().parents[2]
s=(ROOT/'src/nv2a/nv2a_pgraph_d3d11.c').read_text()
a=s.index('static int pgraph_sky_screen_depth_stage(')
body=s[a:s.index('\nstatic void note_guest_surface_resolve',a)]
harness=r'''
#include <stdint.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "nv2a_regs.h"
enum {NV2A_TEXMODE_2D=1,NV2A_TEXMODE_DPNDNT_GB=0x10};
typedef struct {int tex_mode[4],input_tex[4];} NV2ACombinerState;
typedef struct {unsigned valid,offset,width,height,logical_width,logical_height;uint64_t write_serial;} Surface;
typedef Surface GuestDepthSurface;
typedef Surface GuestColorSurface;
static struct {Surface *bound_depth,*bound_color;unsigned host_vsh_hash,depth_test,depth_write;struct {unsigned format,offset,enabled,image_rect;}tex[4];}g_pg;
static unsigned resolved_texture_offset(unsigned n){return g_pg.tex[n].offset;}
static void get_guest_texture_dimensions(unsigned f,unsigned r,unsigned c,unsigned*w,unsigned*h){*w=r>>16;*h=r&65535;}
''' + body + r'''
static Surface d,c;static NV2ACombinerState st;
static void setup(void){
 memset(&g_pg,0,sizeof(g_pg));memset(&st,0,sizeof(st));memset(&d,0,sizeof(d));
 d.valid=1;d.offset=0x2980000;d.width=1440;d.height=1080;d.logical_width=640;d.logical_height=480;d.write_serial=123;c=d;c.offset=0x2790000;
 g_pg.bound_depth=&d;g_pg.bound_color=&c;g_pg.host_vsh_hash=0x47F668F0;g_pg.depth_test=1;
 g_pg.tex[1].enabled=1;g_pg.tex[1].offset=d.offset;g_pg.tex[1].format=NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8B8G8R8<<8;g_pg.tex[1].image_rect=(640<<16)|480;
 st.tex_mode[1]=NV2A_TEXMODE_2D;st.tex_mode[2]=NV2A_TEXMODE_DPNDNT_GB;st.input_tex[2]=1;
}
#define REJECT(change) do{setup();change;assert(pgraph_sky_screen_depth_stage(&st)==0);}while(0)
int main(void){
 setup();assert(pgraph_sky_screen_depth_stage(&st)==2);
 Surface before=d;assert(pgraph_sky_screen_depth_stage(&st)==2);assert(!memcmp(&d,&before,sizeof(d)));
 REJECT(g_pg.host_vsh_hash=0);REJECT(g_pg.bound_depth=0);REJECT(g_pg.bound_color=0);
 REJECT(d.valid=0);REJECT(d.write_serial=0);REJECT(g_pg.depth_test=0);REJECT(g_pg.depth_write=1);
 REJECT(d.width++);REJECT(d.height++);REJECT(g_pg.tex[1].offset+=4096);REJECT(g_pg.tex[1].enabled=0);
 REJECT(g_pg.tex[1].format=NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8R8G8B8<<8);
 REJECT(st.tex_mode[1]=0);REJECT(st.tex_mode[2]=0);REJECT(st.input_tex[2]=0);
 REJECT(c.logical_width++);REJECT(c.logical_height++);
 puts("PASS: sky depth registration enabled only for matching read-only depth view; 18 incompatible states excluded");
}
'''
with tempfile.TemporaryDirectory(prefix='mercs-sky-depth-') as tmp:
 p=Path(tmp);(p/'test.c').write_text(harness)
 subprocess.run(['C:/MinGW/bin/gcc.exe','-std=c11','-O2','-I',str(ROOT/'src/nv2a'),'-I',str(ROOT/'src/d3d'),str(p/'test.c'),'-o',str(p/'test.exe')],check=True)
 subprocess.run([str(p/'test.exe')],check=True)
