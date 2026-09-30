"""Exercise actual lifted retail Doppler functions against native x87 arithmetic.
No audio behavior is replaced. Covers zero/approach/recede, saturation, factors,
listener/source projection, integer rounding and guest stack/FPU balance.
"""
from pathlib import Path
import re,shutil,subprocess,sys,tempfile
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT))
from tools.recomp import config
from tools.recomp.generated_test_utils import generated_text_containing

def body(name):
 s=generated_text_containing('void '+name+'(void)')
 return re.search(r'void '+name+r'\(void\)\n\{.*?\n\}',s,re.S)[0]

def main():
 names=['sub_002A2959','sub_002A07F2','sub_002A0495']
 code='\n'.join(body(n) for n in names)
 xbe=ROOT/'game_files/mercenaries-retail/default.xbe';raw=xbe.read_bytes();config.configure_from_xbe(str(xbe))
 init=[]
 for a in [0x2dc098,0x2a8414,0x302f24,0x302f20,0x2dc08c]:
  off=config.va_to_file_offset(a)
  if off is None:raise ValueError(f'Missing original constant {a:x}')
  init.append(f'MEM32(0x{a:x})=0x{int.from_bytes(raw[off:off+4],"little"):x}u;')
 fixture='#define RECOMP_GENERATED_CODE\n#include "'+(ROOT/'ports/mercenaries/src/recomp/recomp_types.h').as_posix()+'"\n'+r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
uint32_t g_eax,g_ecx,g_edx,g_esp,g_ebx,g_esi,g_edi,g_seh_ebp,g_fp_top;
ptrdiff_t g_xbox_mem_offset;
double g_fp_stack[8];
uint16_t g_x87_control_word=0x037f,g_x87_status_word;
static unsigned char memory[0x4000000];
#undef RECOMP_TRACE_FUNC
#define RECOMP_TRACE_FUNC(a) ((void)0)
'''+code+r'''
enum {STACK=0x3e0000,OUT=0x10000,LV=0x10100,SV=0x10200,DIR=0x10300};
static int32_t original_pitch(float distance,float doppler,float relative) {
 long double product=(long double)distance*doppler*relative;
 if(product==0)return 0;
 if(product>=342)return -32767;
 if(product<=-342)return 4096;
 float value=(float)(1.0L-product*(long double)MEMF(0x302f20));
 float scale=4096;int32_t out;
 __asm__ volatile("flds %1; flds %2; fyl2x; fistpl %0" : "=m"(out):"m"(scale),"m"(value):"st","st(1)");
 return out;
}
static int32_t lifted_pitch(float d,float f,float v) {
 esp=STACK;g_fp_top=0;g_seh_ebp=0xABCDEF;
 esp-=20;MEM32(esp)=0;MEMF(esp+4)=d;MEMF(esp+8)=f;MEMF(esp+12)=v;MEM32(esp+16)=OUT;
 sub_002A07F2();assert(esp==STACK&&g_fp_top==0);
 return (int32_t)MEM32(OUT);
}
int main(void) {
 g_xbox_mem_offset=(ptrdiff_t)memory;
'''+ '\n'.join(init)+r'''
 unsigned cases=0,failed=0;int max_error=0;
 const float factors[]={0,.01f,.5f,1,2,10};
 for(unsigned a=0;a<6;a++)for(unsigned b=0;b<6;b++)for(int i=-8000;i<=8000;i++) {
  float velocity=i*.05f;
  int got=lifted_pitch(factors[a],factors[b],velocity);
  int want=original_pitch(factors[a],factors[b],velocity);
  int error=abs(got-want);if(error>max_error)max_error=error;
  if(error){if(failed<3)printf("mismatch d=%g f=%g v=%g got=%d expected=%d\n",factors[a],factors[b],velocity,got,want);failed++;}cases++;
 }
 assert(lifted_pitch(1,1,0)==0);
 assert(lifted_pitch(1,1,-31)>0&&lifted_pitch(1,1,31)<0);
 unsigned projection_cases=0;
 for(unsigned mode=0;mode<2;mode++)for(int i=-80;i<=80;i++)for(int j=-8;j<=8;j++) {
  float l[3]={i*.4f,j*.1f,i*.2f},s[3]={j*.4f,i*.05f,-j*.2f},dir[3]={.6f,0,.8f};
  for(int k=0;k<3;k++){MEMF(LV+4*k)=l[k];MEMF(SV+4*k)=s[k];MEMF(DIR+4*k)=dir[k];}
  esp=STACK-24;MEM32(esp)=0;MEM32(esp+4)=mode;MEM32(esp+8)=LV;MEM32(esp+12)=SV;MEM32(esp+16)=DIR;MEM32(esp+20)=OUT;g_fp_top=0;
  sub_002A0495();assert(esp==STACK&&g_fp_top==0);
  long double want=0;for(int k=0;k<3;k++)want+=((long double)s[k]-(mode==1?0:l[k]))*dir[k];
  assert(fabs(MEMF(OUT)-(float)want)<.00001);projection_cases++;
 }
 printf("Doppler pitch %u cases, %u mismatches, max error %d; velocity projection %u cases passed\n",cases,failed,max_error,projection_cases);
 printf("31m/s approaching pitch=%d ratio=%g; receding pitch=%d ratio=%g\n",lifted_pitch(1,1,-31),pow(2,lifted_pitch(1,1,-31)/4096.),lifted_pitch(1,1,31),pow(2,lifted_pitch(1,1,31)/4096.));
 return failed?1:0;
}
'''
 with tempfile.TemporaryDirectory(prefix='mercs-doppler-') as d:
  p=Path(d);(p/'check.c').write_text(fixture,encoding='utf-8');cc=shutil.which('gcc') or 'C:/msys64/mingw64/bin/gcc.exe'
  subprocess.run([cc,'-O2','-fno-strict-aliasing',str(p/'check.c'),'-lm','-o',str(p/'check.exe')],check=True)
  subprocess.run([str(p/'check.exe')],check=True)
if __name__=='__main__':main()
