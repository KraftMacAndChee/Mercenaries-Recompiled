"""Execute the retail camera-shake matrix path against the original equations.
Characterizes frame-rate dependence; does not install a shake policy change.
"""
from pathlib import Path
import re,sys,subprocess,tempfile
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT))
from tools.recomp import config
from tools.recomp.generated_test_utils import generated_text_containing

def body(name,text):
 m=re.search(r'(?:static )?void '+name+r'\(void\)\n\{.*?\n\}',text,re.S)
 assert m,name
 return m[0]
names=['sub_00011D80','sub_0022F74C','sub_00098FF0']
code='\n'.join(body(n,generated_text_containing('void '+n+'(void)')) for n in names)
manual=(ROOT/'ports/mercenaries/src/recomp_manual.c').read_text()
code=body('sub_0022F5A7',manual)+'\n'+code
xbe=ROOT/'game_files/mercenaries-retail/default.xbe';raw=xbe.read_bytes();config.configure_from_xbe(str(xbe));init=[]
for a in sorted(set(int(x,16) for x in re.findall(r'MEMF\(0x([0-9A-F]+)\)',code))):
 o=config.va_to_file_offset(a);assert o is not None
 init.append(f'MEM32(0x{a:X})=0x{int.from_bytes(raw[o:o+4],"little"):X}u;')
fixture='#define RECOMP_GENERATED_CODE\n#include "'+(ROOT/'ports/mercenaries/src/recomp/recomp_types.h').as_posix()+'"\n'+r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
uint32_t g_eax,g_ecx,g_edx,g_esp,g_ebx,g_esi,g_edi,g_seh_ebp,g_fp_top;
ptrdiff_t g_xbox_mem_offset;double g_fp_stack[8];
float g_xmm0[4],g_xmm1[4],g_xmm2[4],g_xmm3[4],g_xmm4[4],g_xmm5[4],g_xmm6[4],g_xmm7[4];
static unsigned char memory[0x800000];
#undef RECOMP_TRACE_FUNC
#define RECOMP_TRACE_FUNC(a) ((void)0)
static uint32_t guest_u32(uint32_t a){return MEM32(a);}
static float merc_guest_f32(uint32_t a){return MEMF(a);}
static void merc_guest_store_f32(uint32_t a,float f){MEMF(a)=f;}
'''+code+r'''
enum{STATE=0x10000,MAT=0x20000,STACK=0x3e0000};
static void identity(unsigned a){for(unsigned i=0;i<16;i++)MEMF(a+4*i)=i%5==0?1.f:0.f;}
static void step(float dt){ecx=STATE;esp=STACK;esi=0x12345678;ebx=0x23456789;edi=0x3456789a;g_seh_ebp=0x456789ab;MEM32(esp+4)=MAT;MEMF(esp+8)=dt;MEM32(esp+12)=0;sub_00098FF0();assert(esp==STACK+16&&esi==0x12345678&&ebx==0x23456789&&edi==0x3456789a&&g_seh_ebp==0x456789ab&&g_fp_top==0);}
int main(void){g_xbox_mem_offset=(ptrdiff_t)memory;
'''+''.join(init)+r'''
identity(0x6438e0);unsigned cases=0;
for(unsigned fps=30;fps<=120;fps*=2)for(unsigned i=0;i<1024;i++){
 float dt=1.f/fps,delay=(i%5==0)?dt*1.5f:0,duration=(i%7==0)?0:.01f+(i%97)/64.f,max=(i%13-6.f)/4.f,roll=(i%91-45.f)*3.14159265f/180;
 identity(MAT);MEMF(MAT)=cosf(roll);MEMF(MAT+4)=sinf(roll);MEMF(MAT+16)=-sinf(roll);MEMF(MAT+20)=cosf(roll);MEMF(MAT+48)=100;MEMF(MAT+52)=15;MEMF(MAT+56)=-20;
 float original[16];memcpy(original,memory+MAT,64);MEMF(STATE+0x54)=delay;MEMF(STATE+0x58)=duration;MEMF(STATE+0x5c)=max;
 float expected_delay=delay>0?delay-dt:delay,expected_duration=delay<=0&&duration>0?duration-dt*.5f:duration,expected_max=delay<=0&&duration>0?-max:max;
 float angle=delay<=0&&duration>0?expected_duration*expected_max*.02f:0;
 float rotation[16]={1,0,0,0,0,cosf(angle),sinf(angle),0,0,-sinf(angle),cosf(angle),0,0,0,0,1};
 step(dt);
 assert(MEMF(STATE+0x54)==expected_delay&&MEMF(STATE+0x58)==expected_duration&&MEMF(STATE+0x5c)==expected_max);
 for(unsigned row=0;row<4;row++)for(unsigned col=0;col<4;col++){double expected=0;for(unsigned k=0;k<4;k++)expected+=(double)rotation[row*4+k]*original[k*4+col];assert(fabs(MEMF(MAT+4*(row*4+col))-expected)<.000002);}
 cases++;
}
for(unsigned fps=30;fps<=120;fps*=2){MEMF(STATE+0x54)=0;MEMF(STATE+0x58)=2;MEMF(STATE+0x5c)=1;unsigned flips=0;for(unsigned i=0;i<fps;i++){float previous=MEMF(STATE+0x5c);identity(MAT);step(1.f/fps);flips+=previous*MEMF(STATE+0x5c)<0;}assert(flips==fps);printf("retail shake with %u updates/sec: %u sign flips/sec (%u oscillations/sec)\n",fps,flips,flips/2);}
printf("PASS: %u retail shake delay/duration/amplitude and rolled-camera matrix cases; preserved guest ABI\n",cases);return 0;}
'''
with tempfile.TemporaryDirectory(prefix='shake-native-') as t:
 d=Path(t);(d/'fixture.c').write_text(fixture)
 subprocess.run(['C:/MinGW/bin/gcc.exe','-O1','-msse2','-mfpmath=sse','-fno-strict-aliasing',str(d/'fixture.c'),'-o',str(d/'test.exe')],check=True)
 subprocess.run([str(d/'test.exe')],check=True)
