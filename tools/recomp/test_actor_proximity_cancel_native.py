"""Run the actual lifted proximity evaluator with controlled actor dependencies.

Proves the pre-fix cancellation signal and compares the maintained correction.
Also executes the actual event dispatcher through controlled Lua callback boundaries.
Does not claim a live replay of NW_allies1 or validate spore matrix synchronization.
"""
from pathlib import Path
import re
import runpy
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
patches = runpy.run_path(str(ROOT / "ports/mercenaries/scripts/Patch-Generated.py"))["PATCHES"]
patch = next(p for p in patches if p.name == "suppress canceled actor proximity callback")
s = (ROOT / "ports/mercenaries/src/recomp/gen/recomp_0005.c").read_text()
body = re.search(r"void sub_00110850\(void\)\n\{.*?\n\}", s, re.S)[0]
if patch.after in body:
    body = body.replace(patch.after, patch.before)
assert body.count(patch.before) == 1
fixed = body.replace(patch.before, patch.after)
assert patch.before not in fixed and fixed.count(patch.after) == 1
pre = r"""
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
static uint8_t mem[65536];
static uint32_t eax,ebx,ecx,edx,esi,edi,esp,g_seh_ebp,g_fp_top;
static double g_fp_stack[8];
static float xmm0v[4],xmm1v[4],xmm2v[4];
#define xmm0 xmm0v[0]
#define xmm1 xmm1v[0]
#define xmm2 xmm2v[0]
#define MEM32(a) (*(uint32_t *)(mem+(a)))
#define MEM8(a) mem[(a)]
#define MEMF(a) (*(float *)(mem+(a)))
#define RECOMP_TRACE_FUNC(a) ((void)0)
#define PUSH32(s,v) do{uint32_t x=(v);s-=4;MEM32(s)=x;}while(0)
#define POP32(s,v) do{v=MEM32(s);s+=4;}while(0)
#define CMP_NE(a,b) ((a)!=(b))
#define TEST_Z(a,b) (((a)&(b))==0)
#define LO8(a) ((uint8_t)(a))
#define SET_LO8(a,b) ((a)=((a)&0xffffff00u)|(uint8_t)(b))
static void recomp_xmm_loadss(float *v,uint32_t a){v[0]=MEMF(a);v[1]=v[2]=v[3]=0;}
static void recomp_xmm_copy(float *a,float *b){memcpy(a,b,16);}
static unsigned canceled,matrix_reads;
static void sub_001EBC10(void){eax=MEM32(ecx+0x28);esp+=4;}
static void sub_001ECA50(void){
 uint32_t out=MEM32(esp+4);
 for(unsigned i=0;i<3;i++)MEMF(out+0x30+4*i)=MEMF(ecx+8+4*i);
 matrix_reads++;eax=out;esp+=8;
}
static void sub_0010FA80(void){
 assert(MEM32(esp+4)==123);MEM32(0x2000+0xa8)=0xffffffffu;canceled++;esp+=4;
}
static void sub_00015420(void){
 uint32_t v=MEM32(esp+4);
 float x=MEMF(v),y=MEMF(v+4),z=MEMF(v+8);
 g_fp_stack[--g_fp_top&7u]=(double)x*x+(double)y*y+(double)z*z;esp+=4;
}
"""
main = r"""
static unsigned run(float x,float y,float z,int xz,int option,int invalid){
 memset(mem,0,sizeof(mem));canceled=matrix_reads=0;
 MEM32(0x1000)=0x2000;MEM32(0x20a8)=123;
 MEM32(0x1004)=0x3000;MEM32(0x1008)=11;MEM32(0x3028)=11;
 MEM32(0x100c)=0x4000;MEM32(0x1010)=22;MEM32(0x4028)=22;
 MEMF(0x3008)=x;MEMF(0x300c)=y;MEMF(0x3010)=z;
 MEMF(0x1014)=220.0f*220.0f;MEM8(0x1018)=option;MEM8(0x1019)=xz;
 if(invalid==1)MEM32(0x1004)=0;
 if(invalid==2)MEM32(0x100c)=0;
 if(invalid==3)MEM32(0x3028)=12;
 if(invalid==4)MEM32(0x4028)=23;
 ecx=0x1000;esp=0xf004;esi=0x12345678;ebx=0x2468ace0;edi=0x13579bdf;
 g_seh_ebp=0xaabbccdd;g_fp_top=0;
 sub_00110850();
 assert(esp==0xf00c&&esi==0x12345678&&ebx==0x2468ace0&&edi==0x13579bdf);
 assert(g_fp_top==0&&g_seh_ebp==0xaabbccdd);
 assert(canceled==(invalid!=0)&&matrix_reads==(invalid?0:2));
 assert(MEM32(0x20a8)==(invalid?0xffffffffu:123));
 unsigned expected;
 if(invalid)expected=FIXED?0:!option;
 else {
  double d=xz?(double)((float)(x*x)+(float)(z*z)):((double)x*x+(double)y*y+(double)z*z);
  expected=((float)d<220.0f*220.0f)?option:!option;
 }
 assert(LO8(eax)==expected);return LO8(eax);
}
int main(void){
 unsigned count=0;
 float edges[]={0,219.99f,220,220.01f,1000};
 for(unsigned i=0;i<5;i++)for(int o=0;o<2;o++)for(int xz=0;xz<2;xz++)
  for(int inv=0;inv<5;inv++){run(edges[i],10000,0,xz,o,inv);count++;}
 /* Equal threshold is far, even with zero vertical separation. */
 assert(run(220,0,0,1,0,0)==1);
 assert(run(0,10000,0,1,0,0)==0);
 assert(run(0,10000,0,0,0,0)==1);
 uint32_t seed=37;
 for(unsigned i=0;i<100000;i++){
  seed=seed*1664525u+1013904223u;
  float x=(int)(seed%2001)-1000,y=(int)((seed>>4)%2001)-1000,z=(int)((seed>>8)%2001)-1000;
  run(x,y,z,i%2,(i/2)%2,(i/4)%5);count++;
 }
 printf("PASS: %s; %u cases; valid thresholds/XZ/3D, null/GUID cancellation, guest ABI\n",FIXED?"corrected canceled signal suppressed":"baseline canceled far event signals TRUE",count+3);
}
"""
with tempfile.TemporaryDirectory(prefix="actor-proximity-") as d:
    p = Path(d)
    for label, code, enabled in (("baseline",body,0),("fixed",fixed,1)):
        source = p / (label + ".c")
        exe = p / (label + ".exe")
        source.write_text(f"#define FIXED {enabled}\n"+pre+code+main)
        subprocess.run(["C:/MinGW/bin/gcc.exe","-O2","-fno-strict-aliasing",str(source),"-o",str(exe)],check=True)
        subprocess.run([str(exe)],check=True)


# Run the actual retail event switch through its Lua callback boundary too.
# Lua execution and callback argument serialization are controlled dependencies;
# every other event-type branch aborts if selected accidentally.
dispatcher = re.search(r"void sub_001113A0\(void\)\n\{.*?\n\}", s, re.S)[0]
dispatcher_pre = pre.replace("mem[65536]", "mem[0x120000]") + r"""
#include <stdlib.h>
#define CMP_A(a,b) ((uint32_t)(a)>(uint32_t)(b))
#define RECOMP_ITAIL(a) abort()
void recomp_event_abi_checkpoint(uint32_t caller,uint32_t target,
                                uint32_t expected_esp,uint32_t saved_esi,
                                uint32_t saved_edi){
 (void)caller;(void)target;
 assert(esp==expected_esp&&esi==saved_esi&&edi==saved_edi);
}
static unsigned setup_calls, argument_calls, callback_calls, phase;
static void sub_0010FD90(void){
 assert(ecx==0x2000&&phase==0);setup_calls++;phase=1;esp+=4;
}
static void sub_0010E570(void){
 assert(ecx==0x2018&&MEM32(esp+4)==0x5000&&phase==1);
 argument_calls++;phase=2;eax=5;esp+=8;
}
static void sub_0010E000(void){
 assert(ecx==0x2000&&MEM32(esp+4)==5&&phase==2);
 callback_calls++;phase=3;esp+=8;
}
"""
for symbol in sorted(set(re.findall(r"\bsub_[0-9A-F]+(?=\(\))", dispatcher)) - {
    "sub_001113A0", "sub_00110850", "sub_0010FD90", "sub_0010E570", "sub_0010E000"
}):
    dispatcher_pre += f"static void {symbol}(void){{abort();}}\n"
dispatcher_main = r"""
static unsigned run_dispatch(float distance,int option,int invalid,float threshold){
 memset(mem,0,sizeof(mem));canceled=matrix_reads=0;
 setup_calls=argument_calls=callback_calls=phase=0;
 MEM32(0x2004)=0x5000;MEM32(0x2014)=2;MEM32(0x20a8)=123;
 /* Retail jump table entry for actor-to-actor proximity. */
 MEM32(0x111b30+2*4)=0x111451;
 MEM32(0x2018)=0x2000;
 MEM32(0x201c)=0x3000;MEM32(0x2020)=11;MEM32(0x3028)=11;
 MEM32(0x2024)=0x4000;MEM32(0x2028)=22;MEM32(0x4028)=22;
 MEMF(0x3008)=distance;MEMF(0x300c)=10000;MEMF(0x3010)=0;
 MEMF(0x202c)=threshold*threshold;MEM8(0x2030)=option;MEM8(0x2031)=1;
 if(invalid==1)MEM32(0x201c)=0;
 if(invalid==2)MEM32(0x2024)=0;
 if(invalid==3)MEM32(0x3028)=12;
 if(invalid==4)MEM32(0x4028)=23;
 ecx=0x2000;esp=0xf004;MEMF(esp+4)=1.0f/60;
 esi=0x12345678;ebx=0x2468ace0;edi=0x13579bdf;
 g_seh_ebp=0xaabbccdd;MEM32(0)=0xabcdef12;g_fp_top=0;
 sub_001113A0();
 assert(esp==0xf00c&&esi==0x12345678&&ebx==0x2468ace0&&edi==0x13579bdf);
 assert(MEM32(0)==0xabcdef12&&g_seh_ebp==0xaabbccdd&&g_fp_top==0);
 assert(canceled==(invalid!=0)&&matrix_reads==(invalid?0:2));
 unsigned expected=invalid?(FIXED?0:!option):((distance<threshold)?option:!option);
 assert(setup_calls==expected&&argument_calls==expected&&callback_calls==expected);
 assert(phase==expected*3);
 if(invalid)assert(MEM32(0x20a8)==0xffffffffu);
 return callback_calls;
}
int main(void){
 unsigned cases=0,bad_callbacks=0;
 float thresholds[]={80,100,220};
 for(unsigned t=0;t<3;t++)for(int side=-1;side<=1;side++)
  for(int option=0;option<2;option++)for(int invalid=0;invalid<5;invalid++){
   unsigned called=run_dispatch(thresholds[t]+side*.25f,option,invalid,thresholds[t]);
   if(invalid)bad_callbacks+=called;
   cases++;
  }
 assert(bad_callbacks==(FIXED?0:36));
 printf("PASS: %s actual Update dispatcher; %u cases; canceled callbacks=%u; live 80/100/220m XZ thresholds preserved\n",
        FIXED?"corrected":"baseline",cases,bad_callbacks);
}
"""
with tempfile.TemporaryDirectory(prefix="actor-proximity-dispatch-") as d:
    p = Path(d)
    for label, code, enabled in (("baseline",body,0),("fixed",fixed,1)):
        source = p / (label + ".c")
        exe = p / (label + ".exe")
        source.write_text(f"#define FIXED {enabled}\n"+dispatcher_pre+code+dispatcher+dispatcher_main)
        subprocess.run(["C:/MinGW/bin/gcc.exe","-O2","-fno-strict-aliasing",str(source),"-o",str(exe)],check=True)
        subprocess.run([str(exe)],check=True)
