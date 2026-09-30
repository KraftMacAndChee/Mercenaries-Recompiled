"""Execute retail color multiplication and actual lifted x87 conversion together."""
from pathlib import Path
import re, sys, subprocess, tempfile
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT))
from tools.recomp.generated_test_utils import generated_text_containing
names=['sub_002375B4','sub_000EAE80']
bodies='\n'.join(re.search(r'void '+n+r'\(void\)\n\{.*?\n\}',generated_text_containing('void '+n+'(void)'),re.S)[0] for n in names)
pre='#define RECOMP_GENERATED_CODE\n#include "'+(ROOT/'ports/mercenaries/src/recomp/recomp_types.h').as_posix()+'"\n'+r'''
#include <assert.h>
#include <stdio.h>
uint32_t g_eax,g_ecx,g_edx,g_esp,g_ebx,g_esi,g_edi,g_seh_ebp,g_fp_top;
uint16_t g_x87_control_word=0x37f,g_x87_status_word;
ptrdiff_t g_xbox_mem_offset;double g_fp_stack[8];
float g_xmm0[4],g_xmm1[4],g_xmm2[4],g_xmm3[4],g_xmm4[4],g_xmm5[4],g_xmm6[4],g_xmm7[4];
static unsigned char memory[0x400000];
#undef RECOMP_TRACE_FUNC
#define RECOMP_TRACE_FUNC(a) ((void)0)
'''
main=r'''
int main(void){
 g_xbox_mem_offset=(ptrdiff_t)memory;
 unsigned count=0;
 uint32_t inputs[]={0x808b7f6e,0x80b4a795,0xffffffff,0x00112233,0x80706050};
 float factors[]={0,0.075f,0.25f,0.9f,1,2};
 for(unsigned mode=0;mode<4;mode++)for(unsigned depth=0;depth<6;depth++)
 for(unsigned c=0;c<5;c++)for(unsigned f=0;f<6;f++)for(unsigned alignment=0;alignment<4;alignment++){
 g_x87_control_word=0x37f|(mode<<10);g_fp_top=depth;
 for(unsigned i=0;i<8;i++)g_fp_stack[i]=1234+i;
 esp=0x3e0000+4*alignment;uint32_t stack=esp;
 edi=0x12345678;esi=0x23456789;ebx=0x3456789a;g_seh_ebp=0x456789ab;
 MEM32(esp+4)=inputs[c];for(unsigned i=0;i<4;i++)MEMF(esp+8+4*i)=factors[f];
 sub_000EAE80();
 uint32_t want=0;
 for(unsigned i=0;i<4;i++){int n=(int)(((inputs[c]>>(8*i))&255)*((double)factors[f]));if(n>255)n=255;want|=(uint32_t)n<<(8*i);}
 if(eax!=want){printf("FAIL mode=%u depth=%u input=%08X factor=%g got=%08X want=%08X\n",mode,depth,inputs[c],factors[f],eax,want);return 1;}
 assert(esp==stack+4&&edi==0x12345678&&esi==0x23456789&&ebx==0x3456789a&&g_seh_ebp==0x456789ab);
 assert((g_fp_top&7)==depth);
 count++;
 }
 printf("PASS: %u actual lifted color/conversion cases, rounding modes, stack alignments and register preservation\n",count);
}
'''
with tempfile.TemporaryDirectory(prefix='satellite-color-') as d:
 p=Path(d);src=p/'test.c';exe=p/'test.exe';src.write_text(pre+bodies+main)
 subprocess.run(['C:/MinGW/bin/gcc.exe','-O1','-msse2','-mfpmath=sse','-fno-strict-aliasing',str(src),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True,timeout=30)
