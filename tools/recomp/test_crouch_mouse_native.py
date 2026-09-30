"""Production camera-entry branches must release crouch focus on either mouse axis."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'ports/mercenaries/src/recomp/gen/recomp_0002.c').read_text(encoding='utf-8')
force=s[s.index('loc_000901D0: ;'):s.index('loc_000901E5: ;')]
yaw=s[s.index('loc_0009031C: ;'):s.index('    recomp_xmm_zero(xmm0v);',s.index('loc_0009031C: ;'))]
late=[]
for axis,slot in [(1,'0x14'),(0,'0x30')]:
 start=s.index('    { const float _mouse_before = MEMF(esp + '+slot+');')
 late.append(s[start:s.index(' }',start)+2])
pre=r'''
#include <stdint.h>
#include <math.h>
#include <assert.h>
#include <stdio.h>
static unsigned char memory[0x300000];
static unsigned esi=0x20000,ebx=0x20000,ebp=0x20000,esp=0x10000;
static float xmm0v[4];static int dx,dy;
#define xmm0 xmm0v[0]
#define MEMF(a) (*(float*)(memory+(a)))
#define recomp_xmm_loadss(v,a) ((v)[0]=MEMF(a))
static int recomp_controls_mouse_look_pending(void){return dx || dy;}
static float recomp_controls_mouse_delta(unsigned axis,float original){int counts=axis?dy:dx;if(axis)dy=0;else dx=0;return original+counts*.002f;}
'''
body='static int pitch(void){'+force+'return 1;loc_0009025B: MEMF(esi+0xD74)=0;return 0;loc_00090266: return 0;}\nstatic void yaw(void){'+yaw+'}\n'
body+='static void late_pitch(void){'+late[0]+'}\nstatic void late_yaw(void){'+late[1]+'}\n'
main=r'''
int main(void){MEMF(0x2DC098)=0;
 for(int order=0;order<2;order++)for(int x=-1;x<=1;x++)for(int y=-1;y<=1;y++){
  dx=x;dy=y;MEMF(esi+0xD74)=5;
  int forced;
  if(order){yaw();dx=0;forced=pitch();dy=0;}else{forced=pitch();dy=0;yaw();dx=0;}
  assert(forced==!(x||y));assert(MEMF(esi+0xD74)==((x||y)?0.f:5.f));
 }
 for(int axis=0;axis<2;axis++)for(int value=-1;value<=1;value++){
  dx=dy=0;MEMF(esi+0xD74)=5;assert(pitch()==1);yaw(); /* no input at early check */
  MEMF(esp+0x14)=MEMF(esp+0x30)=0; /* raw packet arrives now */
  if(axis){dy=value;late_pitch();}else{dx=value;late_yaw();}
  assert(MEMF(esi+0xD74)==(value?0.f:5.f));assert(!dx && !dy);
 }
 puts("crouch camera: both mouse axes cancel focus in either update order; no-input focus retained");}
'''
with tempfile.TemporaryDirectory(prefix='merc-crouch-') as d:
 p=Path(d);(p/'test.c').write_text(pre+body+main)
 subprocess.run(['C:/MinGW/bin/gcc.exe','-std=c11','-O2',str(p/'test.c'),'-o',str(p/'test.exe')],check=True)
 subprocess.run([str(p/'test.exe')],check=True)
