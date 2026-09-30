"""Execute positioned HUD vertex/scissor correction without altering other fields."""
from pathlib import Path
import shutil,subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[2]
class HudProportions(unittest.TestCase):
 def test_production_brush_geometry(self):
  s=(ROOT/'ports/mercenaries/src/recomp_ui.c').read_text(encoding='utf-8-sig')
  s=s.replace('#include "recomp/recomp_types.h"','').replace('#include "recomp_options.h"','')
  pre=r'''
#include <stdint.h>
#include <assert.h>
#include <stdio.h>
static unsigned char memory[0x800000];
#define MEM32(a) (*(uint32_t*)(memory+(a)))
#define MEM16(a) (*(uint16_t*)(memory+(a)))
#define MEMF(a) (*(float*)(memory+(a)))
static unsigned aspect;
static unsigned render_height=480;
static void recomp_options_internal_resolution_size(uint32_t*w,uint32_t*h){if(w)*w=render_height*4/3;if(h)*h=render_height;}
static void recomp_options_presentation_aspect(uint32_t*w,uint32_t*h){
 static const unsigned r[4][2]={{4,3},{16,9},{16,10},{21,9}};*w=r[aspect][0];*h=r[aspect][1];
}
'''
  post=r'''
int main(void){
 const uint32_t brush=0x361FC0,vt=0x2F0000,canvas=0x364028,vb=0x400000,prims=0x410000;
 for(aspect=0;aspect<4;aspect++)for(unsigned ref=0;ref<9;ref++){
  unsigned char before[160],p_before[28];
  memset(memory,0,sizeof(memory));memset(anchors,0,sizeof(anchors));scope_depth=0;
  MEM32(brush)=vt;MEM32(brush+4)=canvas;MEM32(vt+0x18)=0x20AB50;
  MEMF(0x3015B4)=0;MEMF(0x3015BC)=320;MEMF(0x3015C0)=640;
  MEMF(canvas+0x3C)=1;MEMF(brush+0x1C)=472;
  recomp_ui_record_position(brush,-58,ref);
  float anchor=-58.0f+(float)(ref%3)*320.0f,scale;
  uint32_t w,h;recomp_options_presentation_aspect(&w,&h);scale=4.0f*h/(3.0f*w);
  MEM32(0x7AC800)=vb;MEM32(0x7AC804)=vb+40;MEM32(0x7AC810)=prims;MEM32(0x7AC814)=1;
  memset(memory+vb,0x5A,160);memset(memory+prims,0x3C,84);
  for(unsigned i=0;i<8;i++)MEMF(vb+i*20)=anchor+(float)i*10;
  memcpy(before,memory+vb,160);memcpy(p_before,memory+prims,28);
  recomp_ui_begin_brush(brush);
  for(render_height=480;render_height<=2160;render_height+=240){
   float factor=recomp_ui_outline_scale(1,1,0);
   float coverage=scale*(float)render_height/480;
   assert(fabsf(factor-(coverage<1?1/coverage:1))<.00001f);
   assert(recomp_ui_outline_scale(1,0,1)==1);
   assert(recomp_ui_outline_scale(4,1,0)==1);
  }
  render_height=480;
  MEM32(0x7AC804)=vb+120;MEM32(0x7AC814)=2;
  MEM16(prims+28+16)=480;MEM16(prims+28+20)=580;
  recomp_ui_end_brush();
  for(unsigned i=0;i<8;i++){
   float expected=anchor+i*10*(i>=2&&i<6?scale:1);
   assert(fabsf(MEMF(vb+i*20)-expected)<0.0002f);
   assert(!memcmp(memory+vb+i*20+4,before+i*20+4,16));
  }
  assert(!memcmp(memory+prims,p_before,28));
  assert(MEM16(prims+28+16)==(uint16_t)(int16_t)floorf(anchor+(480-anchor)*scale));
  assert(MEM16(prims+28+20)==(uint16_t)(int16_t)ceilf(anchor+(580-anchor)*scale));
  if(!aspect)assert(!memcmp(memory+vb,before,160));
  /* Unregistered overlays, reticles and 3D brushes must not be changed. */
  memcpy(before,memory+vb,160);MEM32(0x7AC804)=vb;
  recomp_ui_begin_brush(0x3628C0);MEM32(0x7AC804)=vb+160;recomp_ui_end_brush();
  assert(!memcmp(memory+vb,before,160));
  MEM32(0x7AC804)=vb;recomp_ui_begin_brush(brush);
  MEM32(0x7AC800)=vb+4096;MEM32(0x7AC804)=vb+160;recomp_ui_end_brush();
  assert(!memcmp(memory+vb,before,160));
  /* Every PDA layer uses the same center so map and bezel stay registered. */
  MEM32(0x342F80)=vt;MEM32(0x342F84)=0x342F30;
  MEM32(0x7AC800)=vb;MEM32(0x7AC804)=vb;MEM32(0x7AC814)=0;
  MEMF(vb)=220;recomp_ui_begin_brush(0x342F80);
  MEM32(0x7AC804)=vb+20;recomp_ui_end_brush();
  assert(fabsf(MEMF(vb)-(320-100*scale))<0.0001f);
  /* The retail backing scales with the PDA; do not expand its safe-area
   * inset over the world. Textures, color and partial panels stay intact. */
  for(unsigned variant=0;variant<6;variant++){
   uint32_t pb=variant==4?0x34300C:0x342F80;
   MEM32(pb)=vt;MEM32(pb+4)=0x342F30;
   MEM32(0x7AC800)=vb;MEM32(0x7AC804)=vb;MEM32(0x7AC810)=prims;MEM32(0x7AC814)=0;
   memset(memory+prims,0,56);MEM32(prims+8)=variant==1?123:0;MEM16(prims+26)=4;
   float coords[5][2]={{0,0},{640,0},{0,480},{640,480},{240,240}};
   if(variant==3)coords[1][0]=coords[3][0]=400;
   if(variant==5){coords[0][0]=coords[2][0]=17.8711f;coords[1][0]=coords[3][0]=627.8711f;coords[0][1]=coords[1][1]=-16.75f;coords[2][1]=coords[3][1]=486.1f;}
   for(unsigned j=0;j<5;j++){MEMF(vb+j*20)=coords[j][0];MEMF(vb+j*20+4)=coords[j][1];MEM32(vb+j*20+8)=variant==2?0x80112233:0x80000000;MEM32(vb+j*20+12)=0x3E000000u;MEM32(vb+j*20+16)=0x3F000000u;}
   recomp_ui_begin_brush(pb);MEM32(0x7AC804)=vb+100;MEM32(0x7AC814)=1;recomp_ui_end_brush();
   for(unsigned j=0;j<5;j++){
    float expected=320+(coords[j][0]-320)*scale;
    float expected_y=coords[j][1];
    assert(fabsf(MEMF(vb+j*20)-expected)<0.0002f);assert(MEMF(vb+j*20+4)==expected_y);assert(MEM32(vb+j*20+8)==(variant==2?0x80112233:0x80000000));assert(MEM32(vb+j*20+12)==0x3E000000u&&MEM32(vb+j*20+16)==0x3F000000u);
   }
  }
 }

 /* The sniper reticle's physical proportions must match 4:3 at every aspect,
  * while its outer shading still reaches the screen edges. Other scope types
  * and the ordinary reticle remain on their independent rendering paths. */
 for(aspect=0;aspect<4;aspect++)for(unsigned type=0;type<5;type++){
  memset(memory,0,sizeof(memory));memset(anchors,0,sizeof(anchors));scope_depth=0;
  const uint32_t scope=0x35CDC8,scope_vt=0x2EC264;
  MEM32(scope)=scope_vt;MEM32(scope+4)=0x364170;MEM32(scope+0x2C)=type;
  MEM32(scope_vt+0x18)=0x20AB50;
  MEM32(0x7AC800)=vb;MEM32(0x7AC804)=vb;MEM32(0x7AC810)=prims;
  float xs[]={-.5f,0,120,220,320,420,520,639.5f,640};
  unsigned char before[sizeof(xs)/sizeof(xs[0])*20];
  memset(memory+vb,0x5A,sizeof(before));
  for(unsigned j=0;j<9;j++)MEMF(vb+20*j)=xs[j];
  memcpy(before,memory+vb,sizeof(before));
  MEM16(prims+16)=0;MEM16(prims+20)=640;
  uint32_t w,h;recomp_options_presentation_aspect(&w,&h);float scale=4.f*h/(3.f*w);
  recomp_ui_begin_brush(scope);MEM32(0x7AC804)=vb+sizeof(before);MEM32(0x7AC814)=1;recomp_ui_end_brush();
  for(unsigned j=0;j<9;j++){
   float expected=type==0 && j>=2 && j<=6?320+(xs[j]-320)*scale:xs[j];
   assert(fabsf(MEMF(vb+20*j)-expected)<.0001f);
   assert(!memcmp(memory+vb+20*j+4,before+20*j+4,16));
  }
  assert(MEM16(prims+16)==0 && MEM16(prims+20)==640);
  if(!type)assert(fabsf((MEMF(vb+120)-MEMF(vb+40))*(float)w/h/(4.f/3.f)-400)<.001f);
 }
 /* Menu text, highlight, title and local scissor use one translated origin.
  * Prompt glyphs must not receive their separate aspect correction as well. */
 for(aspect=0;aspect<4;aspect++)for(unsigned shifted=0;shifted<2;shifted++){
  memset(memory,0,sizeof(memory));scope_depth=0;
  uint32_t menu=0x42EA1C,menu_vt=0x2F55A0,menu_canvas=0x42E3F8;
  MEM32(menu)=menu_vt;MEM32(menu+4)=menu_canvas;MEM32(menu_vt+0x18)=0x20AB50;
  MEMF(menu+0x1C)=28;MEMF(menu_canvas+0x48)=shifted?100:0;MEMF(menu_canvas+0x3C)=1.25f;MEMF(0x7AB5C8)=.5f;
  float origin=(28+(shifted?100:0))*1.25f+.5f;
  uint32_t w,h;recomp_options_presentation_aspect(&w,&h);float scale=4.f*h/(3.f*w);
  MEM32(0x7AC800)=vb;MEM32(0x7AC804)=vb;MEM32(0x7AC810)=prims;
  MEMF(vb)=origin+100;MEMF(vb+20)=origin+300;
  MEM16(prims+16)=50;MEM16(prims+20)=500;
  recomp_ui_begin_brush(menu);assert(recomp_ui_prompt_aspect()==1.f);
  MEM32(0x7AC804)=vb+40;MEM32(0x7AC814)=1;recomp_ui_end_brush();
  assert(fabsf(MEMF(vb)-(origin+100*scale))<.0001f);
  assert(fabsf(MEMF(vb+20)-(origin+300*scale))<.0001f);
  assert(MEM16(prims+16)==(uint16_t)(int16_t)floorf(origin+(50-origin)*scale));
  assert(MEM16(prims+20)==(uint16_t)(int16_t)ceilf(origin+(500-origin)*scale));
 }
 puts("PASS: 4 aspects x 9 anchors, geometry and clips, unchanged Y/color/UV, separate HUD buffers and overlays; PDA safe areas; sniper edge coverage and aspect-invariant proportions; translated menus and prompt scaling");
}
'''
  with tempfile.TemporaryDirectory() as d:
   p=Path(d)/'test.c';exe=p.with_suffix('.exe');p.write_text(pre+s+post)
   r=subprocess.run([shutil.which('gcc'),'-std=c11','-O2','-fno-strict-aliasing','-Wall','-Werror',str(p),'-o',str(exe)],capture_output=True,text=True)
   self.assertEqual(r.returncode,0,r.stderr)
   subprocess.run([str(exe)],check=True)
if __name__=='__main__':unittest.main()
