"""Check production camera assistance branches with the option on and off."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'ports/mercenaries/src/recomp/gen/recomp_0002.c').read_text(encoding='utf-8')
pitch=s[s.index('loc_0008FDE0: ;'):s.index('    ecx = 0x4140E8;',s.index('loc_0008FDE0: ;'))]
yaw=s[s.index('loc_0009040F: ;'):s.index('    PUSH32(esp, ecx);',s.index('loc_0009040F: ;'))]
pre=r'''
#include <stdint.h>
#include <math.h>
#include <assert.h>
#include <stdio.h>
static unsigned char memory[0x300000];
static unsigned esp=0x10000,esi=0x20000;
static float xmm0v[4],fp[8];static int top=4,enabled;
#define xmm0 xmm0v[0]
#define MEMF(a) (*(float*)(memory+(a)))
#define recomp_xmm_loadss(v,a) ((v)[0]=MEMF(a))
#define fp_push(v) (fp[--top]=(v))
#define fp_top() fp[top]
#define fp_st(i) fp[top+(i)]
static int recomp_controls_aim_assist(void){return enabled;}
'''
body='static float pitch(void){'+pitch+'return xmm0;}\nstatic float yaw(void){'+yaw+'return fp_top();}\n'
main=r'''
int main(void){MEMF(0x2DC08C)=1;MEMF(0x2DC098)=0;
 for(enabled=0;enabled<2;enabled++){
  MEMF(esp+0x10)=.4f;MEMF(esp+0x14)=.2f;
  assert(fabsf(pitch()-(enabled?.12f:.2f))<.00001f);
  MEMF(esp+0x10)=.8f;top=4;fp[top]=.5f;
  assert(fabsf(yaw()-(enabled?.4f:0.f))<.00001f);
 }
 puts("aim assist: original enabled behavior retained; pitch friction and yaw assistance disabled on request");}
'''
with tempfile.TemporaryDirectory(prefix='merc-aim-assist-') as d:
 p=Path(d);(p/'test.c').write_text(pre+body+main)
 subprocess.run(['C:/MinGW/bin/gcc.exe','-std=c11','-O2',str(p/'test.c'),'-o',str(p/'test.exe')],check=True)
 subprocess.run([str(p/'test.exe')],check=True)
