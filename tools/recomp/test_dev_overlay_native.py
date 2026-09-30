"""Exercise the overlay's production drawing with existing frame history."""
from pathlib import Path
import subprocess,tempfile,sys
ROOT=Path(__file__).resolve().parents[2]
body=(ROOT/'ports/mercenaries/src/dev_overlay.c').read_text().replace('#include "d3d8_internal.h"','')
source=r"""
#include <windows.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define GetTickCount64() ((ULONGLONG)GetTickCount())
typedef void ID3D11ShaderResourceView;
static unsigned draws;
static void (*callback)(void);
int recomp_dev_menu_allowed(void){return 1;}
float recomp_freecam_time_scale(void){return .2f;}
void d3d8_SetHostOverlayCallback(void(*p)(void)){callback=p;}
uint32_t d3d8_GetFrameIntervalHistory(uint64_t after,uint32_t*out,uint32_t cap,uint64_t*serial){
 assert(cap>=160);*serial=160;
 for(unsigned i=0;i<160;++i)out[i]=i==145?45000:16667;return 160;
}
BOOL d3d8_CompositeHostOverlay(const void *p,UINT w,UINT h,UINT x,UINT y,UINT lw,UINT lh){
 assert(w==360 && h==128 && x+w<lw && y+h<lh);
 const uint32_t *rgb=p;unsigned bright=0,clear=0,outline=0;
 for(unsigned i=0;i<w*h;++i){
  if((rgb[i]&0xffffff)>0x808080)++bright;
  if(!rgb[i])++clear;
  if(rgb[i]==0xc0000000)++outline;
  unsigned alpha=rgb[i]>>24;
  assert((rgb[i]&255)<=alpha && ((rgb[i]>>8)&255)<=alpha && ((rgb[i]>>16)&255)<=alpha);
 }
 assert(bright>500 && clear>w*h*3/4 && outline>500);
 assert(rgb[0]==0 && rgb[w*h-1]==0);++draws;return TRUE;
}
"""+body+r"""
int main(int argc,char**argv){
 recomp_dev_overlay_init();assert(callback);callback();assert(!draws && !dc);
 recomp_dev_overlay_set(1);callback();assert(draws==1);callback();assert(draws==2);
 if(argc>1){FILE*f=fopen(argv[1],"wb");assert(f);fwrite(pixels,1,W*H*4,f);fclose(f);}
 recomp_dev_overlay_set(0);callback();assert(draws==2);
 puts("PASS: disabled fast path, history-based text/graph, bounded viewport and enable/disable");
}
"""
with tempfile.TemporaryDirectory(prefix='merc-overlay-') as folder:
 d=Path(folder);(d/'test.c').write_text(source);exe=d/'test.exe'
 subprocess.run(['C:/MinGW/bin/gcc.exe','-std=c11','-D_WIN32_WINNT=0x0601','-O2','-I'+str(ROOT/'ports/mercenaries/src'),'-I'+str(ROOT/'src/d3d'),'-I'+str(ROOT/'src/input'),'-I'+str(ROOT/'src'),str(d/'test.c'),'-o',str(exe),'-lgdi32','-luser32'],check=True)
 subprocess.run([str(exe)]+sys.argv[1:],check=True)
