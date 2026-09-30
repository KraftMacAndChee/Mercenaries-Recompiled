"""Check world geometry proportions and vertical FOV across presentation ratios."""
from pathlib import Path
import subprocess,tempfile
ROOT=Path(__file__).resolve().parents[2]
s=(ROOT/'ports/mercenaries/src/recomp_options.c').read_text(encoding='utf-8')
code=s[s.index('static float selected_camera_aspect'):s.index('/* The retail satellite HUD')]
fixture=r"""
#include <math.h>
#include <assert.h>
#include <stdio.h>
static struct {struct {int aspect;} applied;} g_options;
static void recomp_options_init(void){}
"""+code+r"""
int main(void){
 const float presentation[]={4.f/3.f,16.f/9.f,16.f/10.f,21.f/9.f};
 for(unsigned mode=0;mode<4;mode++)for(unsigned n=1;n<15;n++){
  float fov=n*.1f, original=.764f;g_options.applied.aspect=mode;
  float a=recomp_options_perspective_aspect(original);
  float f=recomp_options_perspective_fov(fov,original);
  /* Equal camera-space X/Y lengths must retain their 4:3 pixel ratio. */
  assert(fabsf(a*presentation[mode]-original*presentation[0])<1e-6f);
  assert(fabsf(tanf(f*.5f)*a-tanf(fov*.5f)*original)<1e-6f);
  /* Live refresh and constructor must not apply the widening twice. */
  assert(fabsf(recomp_options_perspective_fov(f,a)-f)<1e-6f);
  if(!mode){assert(a==original);assert(fabsf(f-fov)<1e-6f);}
 }
 puts("PASS: 4:3, 16:9, 16:10 and 21:9 retain identical object proportions and vertical FOV across 14 zoom values; repeat application stable");
}
"""
with tempfile.TemporaryDirectory(prefix='merc-aspect-') as d:
 p=Path(d);(p/'test.c').write_text(fixture)
 subprocess.run(['C:/MinGW/bin/gcc.exe','-O2',str(p/'test.c'),'-o',str(p/'test.exe')],check=True)
 subprocess.run([str(p/'test.exe')],check=True)
