"""Execute the actual CPU blit mirror against guarded guest render allocations."""
import argparse,re,shutil,subprocess,tempfile
from pathlib import Path
root=Path(__file__).resolve().parents[2]
p=argparse.ArgumentParser();p.add_argument('--source',type=Path,default=root/'src/nv2a/nv2a_pgraph_d3d11.c');a=p.parse_args()
s=a.source.read_text(encoding='utf-8');body=s[s.index('static void mirror_guest_fixed_function_blit('):];body=body.split('\nstatic void submit_draw',1)[0]
pre=r"""
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#define NV097_SET_TEXTURE_FORMAT_COLOR 0xff00u
#define NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8R8G8B8 1u
#define NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_X8R8G8B8 2u
#define NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8B8G8R8 3u
static unsigned char ram[0x40000],before[0x40000];
typedef struct {float x,y;} ImmediateOutputVertex;
typedef struct {unsigned char *vram_ptr;size_t vram;} NV2AState;
static NV2AState device={ram,sizeof(ram)};
static struct {uint32_t draw_mode,blend_enable,surface_color_offset,surface_pitch,surface_clip_h,surface_clip_v;struct {uint32_t format,enabled,image_rect,control1;}tex[4];} g_pg;
static NV2AState *nv2a_get_state(void){return &device;}
static size_t memory_region_size(size_t n){return n;}
static uint32_t resolved_texture_offset(unsigned n){(void)n;return 0x1000;}
static void get_guest_texture_dimensions(uint32_t f,uint32_t rect,uint32_t c,uint32_t *w,uint32_t *h){(void)f;(void)c;*w=rect&65535;*h=rect>>16;}
static const char *pgraph_cached_getenv(const char *s){(void)s;return NULL;}
static uint32_t read_le32(const void *p){uint32_t x;memcpy(&x,p,4);return x;}
"""
tail=r"""
int main(void){
 unsigned bad=0,cases=0;
 for(unsigned pitch=256;pitch<=288;pitch+=32)for(unsigned xo=0;xo<=80;xo+=20)for(unsigned yo=0;yo<=80;yo+=20){
  for(unsigned i=0;i<sizeof(ram);i++)ram[i]=(unsigned char)(i*37+(i>>8));memcpy(before,ram,sizeof(ram));
  g_pg.draw_mode=8;g_pg.surface_color_offset=0x10000;g_pg.surface_pitch=pitch;
  g_pg.surface_clip_h=64u<<16;g_pg.surface_clip_v=64u<<16;
  g_pg.tex[0].enabled=1;g_pg.tex[0].format=1u<<8;g_pg.tex[0].image_rect=(16u<<16)|16;g_pg.tex[0].control1=64u<<16;
  ImmediateOutputVertex v[4]={{xo,yo},{xo+16,yo},{xo+16,yo+16},{xo,yo+16}};
  mirror_guest_fixed_function_blit(v,4);int failed=0;
  for(unsigned i=0;i<sizeof(ram);i++){
   int writable=i>=0x10000&&i<0x10000+pitch*64&&(i-0x10000)%pitch<256;
   if(!writable&&ram[i]!=before[i])failed=1;
  }
  // Fully in-bounds blits must retain existing pixels exactly.
  if(xo+16<=64&&yo+16<=64)for(unsigned y=0;y<16;y++)for(unsigned x=0;x<64;x++)
   if(ram[0x10000+(yo+y)*pitch+xo*4+x]!=before[0x1000+y*64+x])failed=1;
  cases++;bad+=failed;
 }

 // Compare the production scaler with the original pixel-order reference,
 // including overlapping allocations, padding, odd ratios and nonzero origin.
 for(unsigned sw=1;sw<=63;sw+=7)for(unsigned sh=1;sh<=61;sh+=11)
 for(unsigned tw=1;tw<=64;tw+=9)for(unsigned th=1;th<=63;th+=13)
 for(unsigned overlap=0;overlap<2;overlap++){
  for(unsigned i=0;i<sizeof(ram);i++)ram[i]=(unsigned char)(i*37+(i>>8));memcpy(before,ram,sizeof(ram));
  unsigned sp=(sw+3)*4,tp=(tw+5)*4,dest=overlap?0x1080:0x10000;
  g_pg.draw_mode=8;g_pg.surface_color_offset=dest;g_pg.surface_pitch=tp;
  g_pg.surface_clip_h=(tw+5)<<16;g_pg.surface_clip_v=(th+3)<<16;
  g_pg.tex[0].enabled=1;g_pg.tex[0].format=1u<<8;g_pg.tex[0].image_rect=(sh<<16)|sw;g_pg.tex[0].control1=sp<<16;
  ImmediateOutputVertex v[4]={{2,1},{tw+2,1},{tw+2,th+1},{2,th+1}};
  for(unsigned y=0;y<th;y++)for(unsigned x=0;x<tw;x++){
   unsigned char pixel[4];memcpy(pixel,before+0x1000+(y*sh/th)*sp+(x*sw/tw)*4,4);
   memcpy(before+dest+(y+1)*tp+(x+2)*4,pixel,4);
  }
  mirror_guest_fixed_function_blit(v,4);cases++;bad+=memcmp(ram,before,sizeof(ram))!=0;
 }

 // The lookup's largest legal width and first rejected width.
 for(unsigned width=4096;width<=4097;width++){
  for(unsigned i=0;i<sizeof(ram);i++)ram[i]=(unsigned char)(i*37+(i>>8));memcpy(before,ram,sizeof(ram));
  g_pg.surface_color_offset=0x10000;g_pg.surface_pitch=width*4;
  g_pg.surface_clip_h=width<<16;g_pg.surface_clip_v=2u<<16;
  g_pg.tex[0].image_rect=(2u<<16)|1;g_pg.tex[0].control1=4u<<16;
  ImmediateOutputVertex v[4]={{0,0},{width,0},{width,2},{0,2}};
  if(width==4096)for(unsigned y=0;y<2;y++)for(unsigned x=0;x<width;x++)
   memcpy(before+0x10000+(y*width+x)*4,before+0x1000+y*4,4);
  mirror_guest_fixed_function_blit(v,4);cases++;bad+=memcmp(ram,before,sizeof(ram))!=0;
 }
 // Invalid geometry must not turn undefined float conversions into RAM writes.
 const float invalid[]={NAN,INFINITY,-INFINITY,1e30f};
 for(unsigned i=0;i<4;i++){
  memcpy(ram,before,sizeof(ram));
  ImmediateOutputVertex v[4]={{0,0},{16,0},{16,16},{0,16}};v[i].x=invalid[i];
  mirror_guest_fixed_function_blit(v,4);cases++;bad+=memcmp(ram,before,sizeof(ram))!=0;
 }
 printf("CPU blit render-allocation guards: %u cases, %u failures\n",cases,bad);return bad?1:0;
}
"""
with tempfile.TemporaryDirectory(prefix='merc-blit-bounds-') as d:
 c=Path(d)/'test.c';exe=Path(d)/'test.exe';c.write_text(pre+body+tail,encoding='utf-8')
 subprocess.run([shutil.which('gcc') or 'C:/MinGW/bin/gcc.exe','-std=c11','-O2',str(c),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
