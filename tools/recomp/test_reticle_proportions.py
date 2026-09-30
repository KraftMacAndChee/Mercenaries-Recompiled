"""Execute maintained reticle corrections at every supported aspect."""
from pathlib import Path
import re,runpy,shutil,subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[2]
class ReticleProportions(unittest.TestCase):
 def test_actual_geometry_arguments_and_independent_aim_points(self):
  ns=runpy.run_path(str(ROOT/'ports/mercenaries/scripts/Patch-Generated.py'))
  source=(ROOT/'ports/mercenaries/src/recomp/gen/recomp_0004.c').read_text(encoding='utf-8-sig')
  patched=ns['patch_reticle_proportions'](source)
  self.assertEqual(ns['patch_reticle_proportions'](patched),patched)
  edits=re.findall(r'    /\* preserve retail reticle proportions \*/\n(.*?)    PUSH32\(esp, 0\); sub_(0020C160|0020A1C0)\(\);',patched,re.S)
  self.assertEqual(len(edits),31)
  self.assertEqual(sum('esi + 0x4C' in e for e,_ in edits),12)
  self.assertEqual(sum('esi + 0x38' in e for e,_ in edits),18)
  options=(ROOT/'ports/mercenaries/src/recomp_options.c').read_text(encoding='utf-8-sig')
  # Compile only these production geometry functions, not adjacent option accessors.
  helpers='\n'.join(re.search(
      r'^(?:static )?float '+name+r'\([^\n]*\) \{.*?^\}', options, re.M | re.S).group(0)
      for name in ('satellite_center_horizontal_scale',
                   'recomp_options_satellite_center_x',
                   'recomp_options_satellite_center_width', 'recomp_options_ui_x'))
  code=r'''
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <math.h>
#include <stdio.h>
static unsigned aspect;
static void recomp_options_presentation_aspect(uint32_t*w,uint32_t*h){
 static const unsigned r[4][2]={{4,3},{16,9},{16,10},{21,9}};*w=r[aspect][0];*h=r[aspect][1];
}
'''+helpers+r'''
static unsigned char mem[1024];static uint32_t esp=64,esi=512;
#define MEMF(a) (*(float*)(mem+(a)))
'''
  for i,(edit,_) in enumerate(edits):code+=f'void edit_{i}(void){{{edit}}}\n'
  code+='int main(void){for(aspect=0;aspect<4;aspect++){\n'
  for i,(edit,target) in enumerate(edits):
   anchor='245.0f' if '0x4C' in edit else '387.0f' if '0x38' in edit else '224.0f'
   code+=f'''
 {{unsigned char before[1024];memset(mem,0x5A,sizeof(mem));
 MEMF(esp)=192;MEMF(esp+4)=107;MEMF(esp+8)=64;
 MEMF(esi+0x4C)=245;MEMF(esi+0x38)=387;
 memcpy(before,mem,sizeof(mem));edit_{i}();
 assert(fabsf(MEMF(esp)-recomp_options_ui_x(192,{anchor}))<0.0001f);
 for(unsigned b=0;b<sizeof(mem);b++){{
  if((b>=esp&&b<esp+4){'||(b>=esp+8&&b<esp+12)' if target=='0020A1C0' else ''})continue;
  assert(mem[b]==before[b]);
 }}
 if(aspect==0)assert(memcmp(mem,before,sizeof(mem))==0);
 }}
'''
  code+=r'''
 float scale=satellite_center_horizontal_scale();
 for(int center=0;center<=640;center+=40){
  assert(recomp_options_ui_x((float)center,(float)center)==(float)center);
  float left=recomp_options_ui_x(center-20.0f,(float)center);
  float right=recomp_options_ui_x(center+20.0f,(float)center);
  assert(fabsf((right-left)/scale-40)<0.0002f);
 }
 }puts("PASS: 31 retail call sites, 4 aspects, independent projected aim points, other arguments unchanged");}
'''
  with tempfile.TemporaryDirectory() as d:
   c=Path(d)/'test.c';exe=Path(d)/'test.exe';c.write_text(code)
   cc=shutil.which('gcc');self.assertIsNotNone(cc)
   subprocess.run([cc,'-std=c11','-O2',str(c),'-o',str(exe)],check=True)
   subprocess.run([str(exe)],check=True)
if __name__=='__main__':unittest.main()
