"""Execute lifted screen-dimmer setters/update/paint with captured brush submissions.

Renderer submission and x87 integer conversion are controlled dependencies; this
checks retail HUD state, color fading, priority, dimensions and guest ABI, not GPU blending.
"""
from pathlib import Path
import re,sys,subprocess,tempfile
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT))
from tools.recomp import config
from tools.recomp.generated_test_utils import generated_text_containing
names=['sub_000F26A0','sub_000F26C0','sub_000F26D0','sub_000F26F0','sub_000F2720','sub_000F2740']
bodies='\n'.join(re.search(r'void '+n+r'\(void\)\n\{.*?\n\}',generated_text_containing('void '+n+'(void)'),re.S)[0] for n in names)
xbe=ROOT/'game_files/mercenaries-retail/default.xbe';raw=xbe.read_bytes();config.configure_from_xbe(str(xbe))
init=[]
for a in sorted(set(int(x,16) for x in re.findall(r'(?:MEMF\(|recomp_xmm_loadss\(\w+, )0x([0-9A-F]+)',bodies))):
    o=config.va_to_file_offset(a);assert o is not None
    init.append(f'MEM32(0x{a:X})=0x{int.from_bytes(raw[o:o+4],"little"):X}u;')
pre='#define RECOMP_GENERATED_CODE\n#include "'+(ROOT/'ports/mercenaries/src/recomp/recomp_types.h').as_posix()+'"\n'+r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
uint32_t g_eax,g_ecx,g_edx,g_esp,g_ebx,g_esi,g_edi,g_seh_ebp,g_fp_top;
ptrdiff_t g_xbox_mem_offset;double g_fp_stack[8];
float g_xmm0[4],g_xmm1[4],g_xmm2[4],g_xmm3[4],g_xmm4[4],g_xmm5[4],g_xmm6[4],g_xmm7[4];
static unsigned char memory[0x400000];
#undef RECOMP_TRACE_FUNC
#define RECOMP_TRACE_FUNC(a) ((void)0)
#define HUD 0x10000
#define STACK 0x3e0000
static unsigned strips,boxes;static uint32_t colors[2],flags[2];
void recomp_screen_dimmer_checkpoint(uint32_t o){}
void recomp_screen_flash_checkpoint(uint32_t o,uint32_t c,uint32_t t,uint32_t f){}
static void sub_0020F910(void){eax=1080;esp+=4;}
static void sub_0020F920(void){eax=1920;esp+=4;}
static void sub_002375B4(void){eax=(int)g_fp_stack[g_fp_top++&7u];esp+=4;}
static void sub_0020A0D0(void){
 assert(ecx==HUD&&strips<2&&MEM32(esp+4)==0);
 colors[strips]=MEM32(esp+8);flags[strips++]=MEM32(esp+12);esp+=24;
}
static void sub_0020A1C0(void){
 assert(ecx==HUD&&MEMF(esp+4)==0&&MEMF(esp+8)==0);
 assert(MEMF(esp+12)==1920&&MEMF(esp+16)==1080);boxes++;esp+=36;
}
'''
main=r'''
static void setup(void){ecx=HUD;esp=STACK;esi=0x12345678;edi=0x23456789;ebx=0x3456789a;g_seh_ebp=0x456789ab;}
static void check(unsigned n){assert(esp==STACK+n&&esi==0x12345678&&edi==0x23456789&&ebx==0x3456789a&&g_seh_ebp==0x456789ab&&g_fp_top==0);}
static void enable(uint32_t c,int flag,int low){setup();MEM32(esp+4)=c;MEM32(esp+8)=flag;MEM32(esp+12)=low;sub_000F26A0();check(16);}
static void paint(void){setup();strips=boxes=0;sub_000F2740();check(4);assert(strips==boxes);}
int main(void){g_xbox_mem_offset=(ptrdiff_t)memory;
INIT
unsigned cases=0;
uint32_t rgb[]={0xFFFF00,0x403000,0x705000,0,0xFFFFFF,0x112233};
float time[]={0,-.1f,.001f,.125f,.5f,1,1.66f,2,3};
for(unsigned c=0;c<6;c++)for(unsigned a=0;a<256;a++)for(unsigned t=0;t<9;t++)for(int f=-1;f<=1;f++){
 uint32_t color=rgb[c]|(a<<24);setup();MEM32(esp+4)=color;MEMF(esp+8)=2;MEM32(esp+12)=f;sub_000F26F0();check(16);
 assert(MEM32(HUD+0x34)==color&&MEMF(HUD+0x38)==2&&MEMF(HUD+0x3c)==2&&MEM32(HUD+0x40)==(uint32_t)f);
 setup();MEMF(esp+4)=2-time[t];sub_000F2720();check(8);
 float actual=MEMF(HUD+0x38);paint();assert(strips==(actual>0));
 if(actual>0){double q=fmin(1,fmax(0,actual/2));int want=(int)(a*q*q);int got=colors[0]>>24;
 assert(abs(want-got)<=1);assert((colors[0]&0xffffff)==rgb[c]);assert(flags[0]==(uint32_t)f);}
 cases++;
}
MEMF(HUD+0x38)=0;
enable(0x80403000,-1,0);enable(0x20705000,0,1);paint();assert(strips==1&&colors[0]==0x80403000&&flags[0]==0xffffffffu);
setup();MEM32(esp+4)=0x20705000;sub_000F26D0();check(8);assert(MEM32(HUD+0x2c)==0x80403000);
setup();MEM32(esp+4)=0x80403000;sub_000F26D0();check(8);assert(MEM32(HUD+0x2c)==0);
enable(0x20705000,0,1);paint();assert(strips==1&&colors[0]==0x20705000&&flags[0]==0);
setup();MEM32(esp+4)=0x80ffff00;MEMF(esp+8)=1.66f;MEM32(esp+12)=0xffffffffu;sub_000F26F0();check(16);paint();assert(strips==2&&colors[0]==0x20705000&&(colors[1]&0xffffff)==0xffff00&&flags[1]==0xffffffffu);
setup();sub_000F26C0();check(4);assert(MEM32(HUD+0x2c)==0);
printf("PASS: %u lifted HUD fade/setter/update/ABI cases; low-priority underwater, damage priority, conditional clear and dual-layer ordering\n",cases);
}
'''.replace('INIT','\n'.join(init))
with tempfile.TemporaryDirectory(prefix='hud-dimmer-') as d:
    p=Path(d);source=p/'test.c';exe=p/'test.exe';source.write_text(pre+bodies+main)
    subprocess.run(['C:/MinGW/bin/gcc.exe','-O1','-msse2','-mfpmath=sse','-fno-strict-aliasing',str(source),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True,timeout=30)
