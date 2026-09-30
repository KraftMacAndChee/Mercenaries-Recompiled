"""Compile the production roll helper against reference-cadence and jitter checks."""
from pathlib import Path
import subprocess,tempfile,re,importlib.util,sys
root=Path(__file__).resolve().parents[2]
manual=(root/'ports/mercenaries/src/recomp_manual.c').read_text()
wrapper=re.search(r'float recomp_turret_camera_roll\(.*?\n\}',manual,re.S)[0]
prelude=r"""
#include <stdlib.h>
#include <stddef.h>
static unsigned char guest[0x12000];
static ptrdiff_t g_xbox_mem_offset=1;
static uint32_t guest_u32(uint32_t p){uint32_t v;memcpy(&v,guest+p,4);return v;}
static void *guest_ptr(uint32_t p){return guest+p;}
"""
source=r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "turret_camera_roll.h"
static uint32_t rng=912;
static float uniform(void){rng=rng*1664525u+1013904223u;return (rng>>8)*(1.0f/16777216.0f);}
static float legacy(float t,float p,float dt){return (float)((double)p+((double)t-p)*fminf(1,fmaxf(0,12.5f*dt)));}
int main(void){
 const float ref=1.0f/30;
 uint32_t vt=0x2E70E4;memcpy(guest+0x10000,&vt,4);
 float worldx=-.4f,worldy=.85f;memcpy(guest+0x10014,&worldx,4);memcpy(guest+0x10024,&worldy,4);
 assert(recomp_turret_camera_roll(0x10000,-.5f,asinf(worldx),.016f)==mercenaries_turret_roll(-.5f,asinf(worldx),worldx,worldy,.016f));
 assert(recomp_turret_camera_roll(0,.2f,.1f,.016f)==legacy(.2f,.1f,.016f));
 vt=0;memcpy(guest+0x10000,&vt,4);
 assert(recomp_turret_camera_roll(0x10000,.2f,.1f,.016f)==legacy(.2f,.1f,.016f));
 for(int i=0;i<100000;i++){
  float q=(uniform()-.5f)*2.8f,k=.1f+.9f*uniform(),t=(uniform()-.5f)*2.8f;
  float x=sinf(q)*k,y=cosf(q)*k,p=asinf(x);
  float a=mercenaries_turret_roll(t,p,x,y,ref),b=legacy(t,p,ref);assert(memcmp(&a,&b,4)==0);
  assert(fabsf(mercenaries_turret_roll(t,p,x,y,0)-q)<3e-7f);
 }
 double worst=0,old_worst=0;
 for(int scene=0;scene<200;scene++){
  float k=.3f+.69f*uniform(),target=(uniform()-.5f)*2.4f;
  float refq=0,q=0,oldq=0;double low=10,high=-10,olow=10,ohigh=-10;
  for(int i=0;i<1200;i++){
   refq=legacy(target,asinf(sinf(refq)*k),ref);
   float dt=(i%5==0)?1.0f/35:((i%3==0)?1.0f/120:1.0f/60);
   q=mercenaries_turret_roll(target,asinf(sinf(q)*k),sinf(q)*k,cosf(q)*k,dt);
   oldq=legacy(target,asinf(sinf(oldq)*k),dt);
   assert(isfinite(q));
   if(i>600){low=fmin(low,q);high=fmax(high,q);olow=fmin(olow,oldq);ohigh=fmax(ohigh,oldq);}
  }
  assert(fabs(q-refq)<2e-5);assert(high-low<2e-5);worst=fmax(worst,high-low);old_worst=fmax(old_worst,ohigh-olow);
 }
 assert(old_worst>.01);
 printf("PASS: 100000 bit-exact 30Hz cases; zero-dt stability; 200 tilted stationary jitter cases, new max range %.9g vs retail %.9g radians\n",worst,old_worst);
 assert(mercenaries_turret_roll(.2f,.1f,.1f,-.8f,.016f)==legacy(.2f,.1f,.016f));
 assert(mercenaries_turret_roll(.2f,.1f,0,0,.016f)==legacy(.2f,.1f,.016f));
 assert(mercenaries_turret_roll(.2f,.1f,.1f,1,.5f)==legacy(.2f,.1f,.5f));
}
'''
spec=importlib.util.spec_from_file_location('turret_patch_rules',root/'ports/mercenaries/scripts/Patch-Generated.py')
module=importlib.util.module_from_spec(spec);sys.modules[spec.name]=module;spec.loader.exec_module(module)
rule=next(p for p in module.PATCHES if p.name=='turret camera reference-cadence roll')
baseline=rule.before+'\n    PUSH32(esp, eax);'
current=(root/'ports/mercenaries/src/recomp/gen/recomp_0003.c').read_text()
assert current.count(rule.after)==1 and rule.before not in current
def roll_block(text):
 start=text.index('loc_000A18E5: ;')
 return text[start:text.index('    PUSH32(esp, eax);',start)]
abi=r"""
static uint32_t esp,ebp,eax,ecx,edi,top;
static double fp[8];
#define MEMF(a) (*(float*)(guest+(a)))
#define PUSH32(s,v) do{uint32_t saved=(v);(s)-=4;memcpy(guest+(s),&saved,4);}while(0)
#define fp_push(v) (fp[--top&7]=(v))
#define fp_top() fp[top&7]
#define fp_pop() (top++)
"""+'\nstatic void before(void){\n'+roll_block(baseline)+'\n}\nstatic void after(void){\n'+roll_block(current)+'\n}\n'
source=source.replace('int main(void){',abi+'\nint main(void){')
source=source.replace(' double worst=0,old_worst=0;',r"""
 vt=0x2E70E4;memcpy(guest+0x10000,&vt,4);
 for(int i=0;i<10000;i++){
  float t=(uniform()-.5f)*2,p=(uniform()-.5f)*2;
  esp=0x1000;ebp=0x2000;edi=0x10000;ecx=0x11223344;top=3;
  MEMF(esp+0x24)=t;MEMF(esp+0x20)=p;MEMF(esp+0x1c)=12.5f*ref;MEMF(ebp+8)=ref;
  fp[3]=73.25;fp[4]=-21.75;before();
  uint32_t expected=guest_u32(0xffc);assert(esp==0xffc&&eax==0x1150&&top==3);
  esp=0x1000;eax=0;after();
  assert(esp==0xffc&&eax==0x1150&&top==3&&ecx==0x11223344&&edi==0x10000&&ebp==0x2000);
  assert(guest_u32(0xffc)==expected&&fp[3]==73.25&&fp[4]==-21.75);
 }
 double worst=0,old_worst=0;
""")
source=source.replace('static uint32_t rng=912;',prelude+'\n'+wrapper+'\nstatic uint32_t rng=912;')
with tempfile.TemporaryDirectory(prefix='mercs-turret-roll-') as tmp:
 p=Path(tmp);(p/'test.c').write_text(source)
 subprocess.run(['C:/MinGW/bin/gcc.exe','-std=c11','-O2','-I',str(root/'ports/mercenaries/src'),str(p/'test.c'),'-o',str(p/'test.exe')],check=True)
 subprocess.run([str(p/'test.exe')],check=True)

