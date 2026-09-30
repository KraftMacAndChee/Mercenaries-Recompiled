"""Execute the full satellite grid using the corrected intrinsic and controlled draw/camera dependencies."""
from pathlib import Path
import re, sys, subprocess, tempfile
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT))
from tools.recomp.generated_test_utils import generated_text_containing
names=['sub_00238146','sub_000FA110']
bodies='\n'.join(re.search(r'void '+n+r'\(void\)\n\{.*?\n\}',generated_text_containing('void '+n+'(void)'),re.S)[0] for n in names)
pre='#define RECOMP_GENERATED_CODE\n#include "'+(ROOT/'ports/mercenaries/src/recomp/recomp_types.h').as_posix()+'"\n'+r'''
#include <assert.h>
#include <stdio.h>
uint32_t g_eax,g_ecx,g_edx,g_esp,g_ebx,g_esi,g_edi,g_seh_ebp,g_fp_top;
uint16_t g_x87_control_word=0x37f,g_x87_status_word;
ptrdiff_t g_xbox_mem_offset;double g_fp_stack[8];
float g_xmm0[4],g_xmm1[4],g_xmm2[4],g_xmm3[4],g_xmm4[4],g_xmm5[4],g_xmm6[4],g_xmm7[4];
static unsigned char memory[0x900000];
#undef RECOMP_TRACE_FUNC
#define RECOMP_TRACE_FUNC(a) ((void)0)
'''
from tools.recomp import config
xbe=ROOT/'game_files/mercenaries-retail/default.xbe';raw=xbe.read_bytes();config.configure_from_xbe(str(xbe))
init=[]
for a in sorted(set(int(x,16) for x in re.findall(r'(?:MEMF\(|MEMD\(|recomp_xmm_loadss\(\w+, )0x([0-9A-F]+)',bodies))):
 o=config.va_to_file_offset(a);assert o is not None
 for k in (0,4):init.append(f'MEM32(0x{a+k:X})=0x{int.from_bytes(raw[o+k:o+k+4],"little"):X}u;')
pre+=r'''
static unsigned draws;
static void sub_00044FF0(void){eax=MEM32(esp+4);esp+=8;}
static void sub_0022F51C(void){uint32_t out=MEM32(esp+4),in=MEM32(esp+8);memcpy((void*)(g_xbox_mem_offset+out),(void*)(g_xbox_mem_offset+in),12);eax=out;esp+=16;}
static void sub_0020D6F0(void){esp+=4;}
static void sub_0020A0D0(void){esp+=24;}
static void sub_0020A0A0(void){esp+=24;}
static void sub_0020C2D0(void){assert(isfinite(MEMF(esp+4))&&isfinite(MEMF(esp+8)));draws++;esp+=16;}
static void sub_0020C980(void){for(unsigned i=0;i<8;i++)assert(isfinite(MEMF(esp+4+4*i)));draws++;esp+=36;}
static void sub_0020BE50(void){esp+=4;}
static void sub_0020BEB0(void){esp+=4;}
'''
main=r'''
int main(void){g_xbox_mem_offset=(ptrdiff_t)memory;
INIT
unsigned cases=0;
for(unsigned top=0;top<8;top++)for(unsigned align=0;align<4;align++)for(int step=-20;step<=20;step++){
 MEM32(0x41410c)=0;MEMF(0x643910)=step*31.125f;MEMF(0x643914)=step*-19.25f;MEMF(0x643918)=17;
 esp=0x3e0000+align*4;uint32_t stack=esp;g_seh_ebp=0x3dfed0;g_fp_top=top;
 for(unsigned i=0;i<8;i++)g_fp_stack[i]=1000+i;
 edi=0x808b7f6e;esi=0x12345678;ebx=0x23456789;ecx=0x10000;
 MEM32(esp+4)=0x10203040;MEM32(esp+8)=0xffffffff;MEMF(esp+12)=193;MEMF(esp+16)=146;draws=0;
 sub_000FA110();
 assert(esp==stack+20&&edi==0x808b7f6e&&esi==0x12345678&&ebx==0x23456789&&g_seh_ebp==0x3dfed0);
 assert(g_fp_top==top&&draws>0);cases++;
}
printf("PASS: %u complete lifted satellite grid calls; moving coordinates, saved text color, stack balance, finite geometry and frame preservation\n",cases);
}
'''.replace('INIT','\n'.join(init))
with tempfile.TemporaryDirectory(prefix='satellite-color-') as d:
 p=Path(d);src=p/'test.c';exe=p/'test.exe';src.write_text(pre+bodies+main)
 subprocess.run(['C:/MinGW/bin/gcc.exe','-O1','-msse2','-mfpmath=sse','-fno-strict-aliasing',str(src),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True,timeout=30)
