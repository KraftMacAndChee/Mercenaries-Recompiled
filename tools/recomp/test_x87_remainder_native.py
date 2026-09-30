"""Check the actual remainder intrinsic against the retail FPREM kernel and long-double reference."""
from pathlib import Path
import re,sys,subprocess,tempfile
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT))
from tools.recomp.generated_test_utils import generated_text_containing
names=['sub_00238146','sub_00238150']
bodies='\n'.join(re.search(r'void '+n+r'\(void\)\n\{.*?\n\}',generated_text_containing('void '+n+'(void)'),re.S)[0] for n in names)
assert '_CIfmod receives' in bodies
pre='#define RECOMP_GENERATED_CODE\n#include "'+(ROOT/'ports/mercenaries/src/recomp/recomp_types.h').as_posix()+'"\n'+r'''
#include <assert.h>
#include <stdio.h>
#include <float.h>
uint32_t g_eax,g_ecx,g_edx,g_esp,g_ebx,g_esi,g_edi,g_seh_ebp,g_fp_top;
uint16_t g_x87_control_word,g_x87_status_word;
ptrdiff_t g_xbox_mem_offset;double g_fp_stack[8];
static unsigned char memory[4096];
#undef RECOMP_TRACE_FUNC
#define RECOMP_TRACE_FUNC(a) ((void)0)
'''
main=r'''
static int same(double a,double b){return (isnan(a)&&isnan(b))||(a==b&&(a!=0||signbit(a)==signbit(b)));}
static double run(double x,double y,unsigned top,unsigned mode,int kernel){
 for(unsigned i=0;i<8;i++)g_fp_stack[i]=1000+i;
 g_fp_top=top;g_fp_stack[top]=y;g_fp_stack[(top+1)&7]=x;
 g_x87_control_word=0x37f|(mode<<10);g_x87_status_word=0;
 esp=3000;edi=0x808b7f6e;esi=0x12345678;ebx=0x23456789;g_seh_ebp=3072;
 if(kernel)sub_00238150();else sub_00238146();
 assert(esp==3004&&edi==0x808b7f6e&&esi==0x12345678&&ebx==0x23456789&&g_seh_ebp==3072);
 assert(g_fp_top==top+1&&g_x87_control_word==(0x37f|(mode<<10)));
 for(unsigned i=2;i<8;i++)assert(g_fp_stack[(top+i)&7]==1000+((top+i)&7));
 if(!kernel&&(isinf(x)||y==0))assert(g_x87_status_word&1);
 return g_fp_stack[(top+1)&7];
}
int main(void){
 g_xbox_mem_offset=(ptrdiff_t)memory;memset(memory,0xa5,sizeof(memory));unsigned cases=0;
 double xs[]={-100000.25,-701,-70,-69.9,-1,-0.0,0.0,0.125,1,69.9,70,701,100000.25,DBL_MAX,DBL_MIN,INFINITY,-INFINITY,NAN};
 double ys[]={-70,-2,-0.0,0.0,0.125,2,70,DBL_MIN,INFINITY,NAN};
 for(unsigned top=0;top<8;top++)for(unsigned mode=0;mode<4;mode++)
 for(unsigned i=0;i<sizeof(xs)/sizeof(*xs);i++)for(unsigned j=0;j<sizeof(ys)/sizeof(*ys);j++){
 double want=(double)fmodl((long double)xs[i],(long double)ys[j]);
 double got=run(xs[i],ys[j],top,mode,0);if(!same(got,want)){printf("FAIL x=%.17g y=%.17g got=%.17g want=%.17g signs=%d/%d top=%u mode=%u\n",xs[i],ys[j],got,want,signbit(got),signbit(want),top,mode);return 1;}
 if(isfinite(xs[i])&&isfinite(ys[j])&&ys[j]!=0){double k=run(xs[i],ys[j],top,mode,1);assert(got==k);}
 cases++;
 }
 for(unsigned i=0;i<sizeof(memory);i++)assert(memory[i]==0xa5);
 printf("PASS: %u remainder cases; retail kernel/reference parity, signed zero, nonfinite inputs, all x87 slots, preserved caller memory/registers/control word\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='x87-remainder-') as d:
 p=Path(d);src=p/'test.c';exe=p/'test.exe';src.write_text(pre+bodies+main)
 subprocess.run(['C:/MinGW/bin/gcc.exe','-O1','-msse2','-mfpmath=sse','-fno-strict-aliasing',str(src),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True,timeout=30)
