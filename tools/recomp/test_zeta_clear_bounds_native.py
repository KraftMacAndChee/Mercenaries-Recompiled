"""Execute the CPU depth-clear mirror against guarded surface allocations."""
import argparse, shutil, subprocess, tempfile
from pathlib import Path
root=Path(__file__).resolve().parents[2]
p=argparse.ArgumentParser();p.add_argument('--source',type=Path,default=root/'src/nv2a/nv2a_pgraph_d3d11.c');a=p.parse_args()
s=a.source.read_text(encoding='utf-8');body=s[s.index('static void mirror_guest_zeta_clear('):].split('/* ======================================================================',1)[0]
pre=r"""
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "nv2a_regs.h"
static unsigned char ram[0x80000],expected[0x80000];
typedef struct {unsigned char *vram_ptr;size_t vram;} NV2AState;
static NV2AState device={ram,sizeof(ram)};
static struct {uint32_t surface_format,surface_pitch,surface_clip_h,surface_clip_v,clear_rect_h,clear_rect_v,surface_zeta_offset,zstencil_clear;} g_pg;
static NV2AState *nv2a_get_state(void){return &device;}
static size_t memory_region_size(size_t n){return n;}
static const char *pgraph_cached_getenv(const char *s){(void)s;return NULL;}
"""
tail=r"""
int main(void){unsigned cases=0,bad=0,guard_bad=0;
 for(unsigned fmt=1;fmt<=2;fmt++)for(unsigned aa=0;aa<3;aa++)for(unsigned origin=0;origin<2;origin++)
 for(unsigned pad=0;pad<2;pad++)for(unsigned start=0;start<3;start++)for(unsigned end=0;end<3;end++)
 for(unsigned alignment=0;alignment<4;alignment++){
  unsigned base=0x10000+alignment;
  unsigned bpp=fmt==2?4:2,sx=aa?2:1,sy=aa==2?2:1;
  unsigned width=32+origin*3,height=24+origin*5,pitch=width*sx*bpp+pad*16;
  unsigned x0=start*20,y0=start*15,x1=x0+end*22,y1=y0+end*17;
  memset(ram,0x5a,sizeof(ram));memcpy(expected,ram,sizeof(ram));
  g_pg.surface_format=(fmt<<4)|(aa<<12);g_pg.surface_pitch=pitch<<16;
  g_pg.surface_clip_h=(32u<<16)|(origin*3);g_pg.surface_clip_v=(24u<<16)|(origin*5);
  g_pg.surface_zeta_offset=base;g_pg.zstencil_clear=0x1234abcd;
  g_pg.clear_rect_h=(x1<<16)|x0;g_pg.clear_rect_v=(y1<<16)|y0;
  for(unsigned y=0;y<height*sy;y++)for(unsigned x=0;x<width*sx;x++)
   if(x/sx>=x0&&x/sx<=x1&&y/sy>=y0&&y/sy<=y1)
    memcpy(expected+base+y*pitch+x*bpp,&g_pg.zstencil_clear,bpp);
  mirror_guest_zeta_clear(3);cases++;bad+=memcmp(ram,expected,sizeof(ram))!=0;
  int corrupt=0;
  for(unsigned i=0;i<sizeof(ram);i++){
   int inside=i>=base&&i<base+pitch*height*sy&&(i-base)%pitch<width*sx*bpp;
   if(!inside&&ram[i]!=0x5a)corrupt=1;
  }
  guard_bad+=corrupt;
 }
 printf("CPU depth clear allocation/pixel oracle: %u cases, %u failures, %u allocation-guard failures\n",cases,bad,guard_bad);return bad?1:0;
}
"""
with tempfile.TemporaryDirectory(prefix='merc-zeta-bounds-') as d:
 c=Path(d)/'test.c';exe=Path(d)/'test.exe';c.write_text(pre+body+tail,encoding='utf-8')
 subprocess.run([shutil.which('gcc') or 'C:/MinGW/bin/gcc.exe','-std=c11','-O2','-I',str(root/'src/nv2a'),str(c),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
