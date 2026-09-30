"""Execute the full lifted spawn-update function along its timer-only path.

Spawn checks are deferred and zone resource updates disabled in the fixture.
Every external call aborts if that isolation is violated; the decrement loop,
expiry, path/zone counts, stack frame and returns are the actual generated C.
"""
from pathlib import Path
import re,subprocess,tempfile
ROOT=Path(__file__).resolve().parents[2]
s=(ROOT/'ports/mercenaries/src/recomp/gen/recomp_0006.c').read_text(encoding='utf-8')
b=re.search(r'void sub_0016BDA0\(void\)\n\{.*?\n\}',s,re.S)[0]
pre='#define RECOMP_GENERATED_CODE\n#include "'+(ROOT/'ports/mercenaries/src/recomp/recomp_types.h').as_posix()+'"\n'+r"""
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
uint32_t g_eax,g_ecx,g_edx,g_esp,g_ebx,g_esi,g_edi,g_seh_ebp,g_fp_top;
ptrdiff_t g_xbox_mem_offset;double g_fp_stack[8];
float g_xmm0[4],g_xmm1[4],g_xmm2[4],g_xmm3[4],g_xmm4[4],g_xmm5[4],g_xmm6[4],g_xmm7[4];
static unsigned char memory[0x400000];
#undef RECOMP_TRACE_FUNC
#define RECOMP_TRACE_FUNC(a) ((void)0)
#undef RECOMP_ICALL_SAFE
#define RECOMP_ICALL_SAFE(a,s) abort()
void recomp_traffic_spawn_checkpoint(uint32_t a,uint32_t b,uint32_t c,uint32_t d,uint32_t e,uint32_t f,uint32_t g){abort();}
"""
# Keep every non-timer dependency fatal, so the test cannot silently substitute
# behavior outside its stated scope.
for name in sorted(set(re.findall(r'\b(sub_[0-9A-F]+)\(\)',b))):
    if name!='sub_0016BDA0':pre+='static void '+name+'(void){abort();}\n'
main=r"""
#define MANAGER 0x10000
#define RECORD (MANAGER+0x18b58)
static void step(float dt){
 ecx=MANAGER;esp=0x3f0000;g_seh_ebp=0x12345678;ebx=0x23456789;esi=0x3456789a;edi=0x456789ab;g_fp_top=0;
 MEMF(esp+4)=dt;MEM32(esp+8)=0x7000;MEMF(RECORD+0x14)=1000000;
 sub_0016BDA0();
 assert(esp==0x3f000c&&g_seh_ebp==0x12345678&&ebx==0x23456789&&esi==0x3456789a&&edi==0x456789ab&&g_fp_top==0);
}
static void init(unsigned count,float timer,unsigned zone){
 memset(memory,0,sizeof(memory));MEM32(MANAGER+0x18b54)=1;MEM32(RECORD)=0x6000;
 MEM32(RECORD+4)=zone?0x2000:0;MEM32(RECORD+0x1c)=count;MEMF(RECORD+0x20)=timer;
 MEM32(0x2270)=count;MEMF(0x2278)=600;
}
int main(void){
 g_xbox_mem_offset=(ptrdiff_t)memory;unsigned cases=0,frames=0;
 float deltas[]={0,1.f/120,1.f/60,1.f/30,.1f,.25f,1,599,600,601,1200};
 float timers[]={0,600,123,-.1f};
 for(unsigned z=0;z<2;z++)for(unsigned c=0;c<4;c++)for(unsigned t=0;t<4;t++)for(unsigned d=0;d<11;d++){
 init(c,timers[t],z);float expected=timers[t];unsigned count=c;
 if(expected>0){expected-=deltas[d];if(expected<=0){expected=0;if(count){--count;if(count&&z)expected=600;}}}
 step(deltas[d]);
 assert(MEMF(RECORD+0x20)==expected&&MEM32(RECORD+0x1c)==count);
 assert(MEM32(0x2270)==(z?count:c));cases++;
 }
 for(unsigned hz=30;hz<=120;hz*=2){
 init(1,600,1);unsigned n=0;float elapsed=0;
 while(MEM32(RECORD+0x1c)){
  step(1.f/hz);elapsed+=1.f/hz;++n;assert(n<hz*601);
 }
 assert(n>=hz*599&&n<=hz*601);assert(MEM32(0x2270)==0&&MEMF(RECORD+0x20)==0);
 printf("%uHz: one 600s cooldown expires after %.6f seconds of supplied dt (%u updates)\n",hz,(double)n/hz,n);frames+=n;
 }
 printf("PASS: %u boundary/countdown cases, %u long-run updates; path/zone expiry and guest ABI\n",cases,frames);
}
"""
with tempfile.TemporaryDirectory(prefix='traffic-countdown-') as d:
 d=Path(d);(d/'test.c').write_text(pre+b+main,encoding='utf-8')
 subprocess.run(['C:/MinGW/bin/gcc.exe','-O2','-fno-strict-aliasing',str(d/'test.c'),'-o',str(d/'test.exe')],check=True,timeout=30)
 subprocess.run([str(d/'test.exe')],check=True,timeout=30)
