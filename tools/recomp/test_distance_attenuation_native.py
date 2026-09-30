"""Check retail fire/NPC distance attenuation against independent native x87 math.
Only the authored inverse-distance path is covered; custom curves are excluded.
No mixer, voice lifetime, DSP or authored settings are changed.
"""
from pathlib import Path
import re,subprocess,sys,tempfile
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT))
from tools.recomp import config
from tools.recomp.generated_test_utils import generated_text_containing

def body(name):
 return re.search(r'void '+name+r'\(void\)\n\{.*?\n\}',generated_text_containing('void '+name+'(void)'),re.S)[0]

def main():
 raw=(ROOT/'game_files/mercenaries-retail/default.xbe').read_bytes()
 config.configure_from_xbe(str(ROOT/'game_files/mercenaries-retail/default.xbe'))
 constants=[]
 for address in [0x2dc098,0x2dc08c,0x2e27fc,0x2f2208]:
  offset=config.va_to_file_offset(address)
  assert offset is not None
  constants.append(f'MEM32(0x{address:x})=0x{int.from_bytes(raw[offset:offset+4],"little"):x}u;')
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
static void sub_002375B4(void){abort();} /* custom curves excluded */
static void sub_0029CC74(void){abort();}
'''+ '\n'.join(body(n) for n in ['sub_0029CC6B','sub_002A0196','sub_002A04E0'])+r'''
enum {STACK=0x3e0000,OUT=0x10000};
static int original(float rolloff,float minimum,float maximum,float distance,int stop) {
 if(distance<=minimum)return 0;
 if(stop&&distance>=maximum)return -10000;
 if(distance>maximum)distance=maximum;
 float adjusted=(float)(((long double)distance/minimum-1)*rolloff);
 if(adjusted<0)return 0;
 long double base=1+(long double)adjusted;
 float scale=-2000,out;
 __asm__ volatile("fldlg2; fldt %1; fyl2x; fmuls %2; fstps %0":"=m"(out):"m"(base),"m"(scale):"st","st(1)");
 return (int)out;
}
static int actual(float r,float mn,float mx,float d,int stop) {
 esp=STACK;g_fp_top=0;g_seh_ebp=0xABCDEF;esi=0x98765;edi=0x87654;
 esp-=36;MEM32(esp)=0;MEMF(esp+4)=r;MEMF(esp+8)=mn;MEMF(esp+12)=mx;
 MEM32(esp+16)=0;MEM32(esp+20)=0;MEMF(esp+24)=d;MEM32(esp+28)=stop;MEM32(esp+32)=OUT;
 sub_002A04E0();
 assert(esp==STACK&&g_fp_top==0&&esi==0x98765&&edi==0x87654);
 return (int)MEM32(OUT);
}
int main(void){g_xbox_mem_offset=(ptrdiff_t)memory;
'''+ '\n'.join(constants)+r'''
 const float ranges[][2]={{15,1000},{15,100000},{35,100000},{1,1000}};
 const float rolloffs[]={0,.1f,.8f,1,3,10};
 unsigned cases=0,mismatches=0;int maximum_error=0;
 for(unsigned pair=0;pair<4;pair++)for(unsigned r=0;r<6;r++)for(int stop=0;stop<2;stop++){
  float mn=ranges[pair][0],mx=ranges[pair][1];
  for(int step=0;step<=10000;step++){
   float d=step*(mx/8000.f);
   int a=actual(rolloffs[r],mn,mx,d,stop),b=original(rolloffs[r],mn,mx,d,stop),error=abs(a-b);
   if(error>maximum_error)maximum_error=error;
   if(error>1){if(mismatches<3)printf("mismatch r=%g min=%g max=%g d=%g stop=%d got=%d expected=%d\n",rolloffs[r],mn,mx,d,stop,a,b);mismatches++;}
   cases++;
  }
  float edges[]={0,mn,nextafterf(mn,0),nextafterf(mn,INFINITY),mx,nextafterf(mx,0),nextafterf(mx,INFINITY)};
  for(unsigned i=0;i<7;i++){int a=actual(rolloffs[r],mn,mx,edges[i],stop),b=original(rolloffs[r],mn,mx,edges[i],stop);assert(abs(a-b)<=1);cases++;}
 }
 assert(actual(3,15,1000,15,0)==0);
 printf("PASS: cases=%u errors_above_0.01dB=%u max_error_hundredth_dB=%d\n",cases,mismatches,maximum_error);
 for(int d=15;d<=120;d*=2)printf("authored fire distance=%dm attenuation=%g dB\n",d,actual(3,15,1000,(float)d,0)/100.f);
 return mismatches?1:0;
}
'''
 with tempfile.TemporaryDirectory(prefix='fire-attenuation-') as tmp:
  p=Path(tmp);(p/'fixture.c').write_text(fixture,encoding='utf-8')
  subprocess.run(['C:/MinGW/bin/gcc.exe','-O2','-std=c11','-fno-strict-aliasing',str(p/'fixture.c'),'-o',str(p/'fixture.exe')],check=True)
  subprocess.run([str(p/'fixture.exe')],check=True)
if __name__=='__main__':main()
