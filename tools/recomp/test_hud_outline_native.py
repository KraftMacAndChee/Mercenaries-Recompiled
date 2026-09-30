"""Exercise the lifted HUD outline builder and its final aspect correction."""
from pathlib import Path
import re,subprocess,tempfile
ROOT=Path(__file__).resolve().parents[2]
s=(ROOT/'ports/mercenaries/src/recomp/gen/recomp_0010.c').read_text()
body=re.search(r'void sub_0020AFA0\(void\)\n\{.*?\n\}',s,re.S)[0]
ui=(ROOT/'ports/mercenaries/src/recomp_ui.c').read_text().replace('#include "recomp/recomp_types.h"','').replace('#include "recomp_options.h"','')
pre='#define RECOMP_GENERATED_CODE\n#include "'+(ROOT/'ports/mercenaries/src/recomp/recomp_types.h').as_posix()+'"\n'+r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
uint32_t g_eax,g_ecx,g_edx,g_esp,g_ebx,g_esi,g_edi,g_seh_ebp,g_fp_top;
ptrdiff_t g_xbox_mem_offset;double g_fp_stack[8];
float g_xmm0[4],g_xmm1[4],g_xmm2[4],g_xmm3[4],g_xmm4[4],g_xmm5[4],g_xmm6[4],g_xmm7[4];
static unsigned char memory[0x800000];
#undef RECOMP_TRACE_FUNC
#define RECOMP_TRACE_FUNC(a) ((void)0)
static unsigned height,aspect,count;static float vertices[16][2];
void recomp_options_internal_resolution_size(uint32_t*w,uint32_t*h){if(w)*w=height*4/3;if(h)*h=height;}
void recomp_options_presentation_aspect(uint32_t*w,uint32_t*h){*w=aspect?16:4;*h=aspect?9:3;}
void sub_0020C160(void){assert(count<16);vertices[count][0]=MEMF(esp+4);vertices[count++][1]=MEMF(esp+8);esp+=12;}
'''
main=r'''
int main(void){g_xbox_mem_offset=(ptrdiff_t)memory;MEMF(0x2DC340)=.5f;MEMF(0x2DC08C)=1;
MEMF(0x7AB614)=MEMF(0x7AB620)=1;
for(aspect=0;aspect<2;aspect++)for(height=480;height<=2160;height+=240){
 float scale=aspect?.75f:1, factor=(float)height/480;
 for(unsigned shape=4;shape<=5;shape++)for(unsigned shift=0;shift<16;shift++){
  const float points[5][2]={{100,100},{200,100},{210,110},{210,140},{100,140}};
  float saved[16][2];unsigned saved_count=0;
  for(unsigned mode=0;mode<2;mode++){
   scope_depth=1;scopes[0].active=mode&&aspect;scopes[0].scale=scale;
   for(unsigned i=0;i<shape;i++){unsigned index=shape==4&&i>=2?i+1:i;MEMF(0x500000+i*8)=points[index][0]+shift/16.f;MEMF(0x500004+i*8)=points[index][1];}
   esp=0x700000;esi=123;edi=234;ebx=345;g_seh_ebp=456;g_fp_top=0;count=0;
   MEMF(esp+4)=1;MEM32(esp+8)=shape;MEM32(esp+12)=0x500000;sub_0020AFA0();
   assert(esp==0x700010&&esi==123&&edi==234&&ebx==345&&g_seh_ebp==456&&g_fp_top==0);
   assert(count==2*(shape+1));
   if(!mode){memcpy(saved,vertices,sizeof(vertices));saved_count=count;continue;}
   assert(count==saved_count);
   for(unsigned i=0;i<count;i++){
    assert(vertices[i][1]==saved[i][1]);
    if(!aspect||factor*scale>=1)assert(vertices[i][0]==saved[i][0]);
   }
   // Left vertical border: after aspect and render scaling it covers a sample
   // for every fractional placement. Old widescreen/native strips do not.
   float left=fminf(vertices[0][0],vertices[1][0])*scale*factor;
   float right=fmaxf(vertices[0][0],vertices[1][0])*scale*factor;
   assert(right-left>=.9999f);
   assert(ceilf(left-.5f)<right-.5f || fabsf(right-left-1)<.0001f);
  }
 }
}
puts("PASS: lifted 4/5-point outlines, 4:3/16:9, fractional positions and resolutions; native coverage, high-resolution geometry and guest ABI");}
'''
with tempfile.TemporaryDirectory(prefix='merc-hud-outline-') as folder:
 p=Path(folder);(p/'test.c').write_text(pre+ui+body+main)
 subprocess.run(['C:/MinGW/bin/gcc.exe','-O1','-fno-strict-aliasing',str(p/'test.c'),'-o',str(p/'test.exe')],check=True)
 subprocess.run([str(p/'test.exe')],check=True)

 # The pre-fix aspect compression must fail the same physical coverage check.
 old_ui=ui.replace('return pixels > 0.0f && pixels < 1.0f ? 1.0f / pixels : 1.0f;', 'return 1.0f;')
 assert old_ui!=ui
 (p/'test.c').write_text(pre+old_ui+body+main)
 subprocess.run(['C:/MinGW/bin/gcc.exe','-O1','-fno-strict-aliasing',str(p/'test.c'),'-o',str(p/'old.exe')],check=True)
 result=subprocess.run([str(p/'old.exe')],capture_output=True)
 assert result.returncode, 'Old subpixel outline was not detected'
 print('PASS: uncorrected widescreen outline reproduces missing coverage')
