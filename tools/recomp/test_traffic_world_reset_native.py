"""A world reset must discard its traffic budget regardless of the OG Bugs setting."""
from pathlib import Path
import re,subprocess,tempfile
ROOT=Path(__file__).resolve().parents[2]
gen=(ROOT/'ports/mercenaries/src/recomp/gen/recomp_0006.c').read_text()
manual=(ROOT/'ports/mercenaries/src/recomp_manual.c').read_text()
policy=(ROOT/'ports/mercenaries/src/recomp_original_bugs.c').read_text()
fn=lambda s,n,t='void':re.search(t+r' '+n+r'\([^)]*\)\s*\{.*?\n\}',s,re.S)[0]
pre='#define RECOMP_GENERATED_CODE\n#include "'+(ROOT/'ports/mercenaries/src/recomp/recomp_types.h').as_posix()+'"\n#include "'+(ROOT/'ports/mercenaries/src/recomp_original_bugs.h').as_posix()+'"\n'+r'''
#include <assert.h>
#include <stdio.h>
uint32_t g_eax,g_ecx,g_edx,g_esp,g_ebx,g_esi,g_edi,g_seh_ebp;
ptrdiff_t g_xbox_mem_offset;
static unsigned char memory[0x400000];
static int og,order;
#define M 0x10000
#undef RECOMP_TRACE_FUNC
#define RECOMP_TRACE_FUNC(a) ((void)0)
#undef RECOMP_ITAIL
#define RECOMP_ITAIL(a) deactivate(a)
static void *guest_ptr(uint32_t a){return memory+a;}
static uint32_t guest_u32(uint32_t a){return MEM32(a);}
static float guest_f32(uint32_t a){return MEMF(a);}
static int xbox_preview_log_enabled(void){return 0;}
#define xbox_preview_log_event(...) ((void)0)
int recomp_options_og_bugs(void){return og;}
static void sub_0016A070(void){assert(ecx==M&&order++==0);esp+=4;}
static void sub_00168E50(void){assert(ecx==M&&order++==1);esp+=4;}
static void sub_00169000(void){assert(ecx==M&&order++==2);esp+=4;}
static void deactivate(uint32_t a){assert(a==0x1234&&ecx==M&&order++==3);esp+=4;}
'''
body=fn(policy,'recomp_original_bug_fix_enabled','int')+fn(manual,'recomp_traffic_stop_network_totals')+fn(gen,'sub_0016A330')
main=r'''
int main(void){
 g_xbox_mem_offset=(ptrdiff_t)memory;
 for(og=0;og<=1;og++)for(unsigned cycle=0;cycle<1000;cycle++){
  memset(memory,0,sizeof(memory));MEM32(M)=0x4000;MEM32(0x4010)=0x1234;
  MEM32(M+0x18b54)=562;MEM32(M+0x28)=47;
  float vehicles=6.865252f+cycle*.0442845f,people=cycle*.1f;
  MEMF(M+0x22d40)=vehicles;MEMF(M+0x22d50)=people;
  MEM32(M+0x22d34)=40;MEM32(M+0x22d44)=80;
  MEM32(M+0x22d38)=3;MEM32(M+0x22d48)=7;
  MEMF(M+0x22d3c)=212;MEMF(M+0x22d4c)=212;
  order=0;ecx=M;esp=0x3e0000;esi=0xabcdef;g_seh_ebp=0x12345678;
  sub_0016A330();assert(esp==0x3e0004&&esi==0xabcdef&&g_seh_ebp==0x12345678&&order==4);
  assert(!MEM32(M+0x18b54)&&!MEM32(M+0x28));
  assert(MEMF(M+0x22d40)==0.0f&&MEMF(M+0x22d50)==0.0f);
  assert(MEM32(M+0x22d34)==40&&MEM32(M+0x22d44)==80);
  assert(MEM32(M+0x22d38)==3&&MEM32(M+0x22d48)==7);
  assert(MEMF(M+0x22d3c)==212&&MEMF(M+0x22d4c)==212);
  /* New routes add to a clean total, not the previous world's budget. */
  MEMF(M+0x22d40)+=7;assert(fabsf(MEMF(M+0x22d40)-7.f)<0.00001f);
 }
 puts("PASS: 2000 shutdown cases; both network budgets reset, correction active with OG Bugs on and off, caps/radii/teardown counts and ABI unchanged");
}
'''
code=pre+body+main
with tempfile.TemporaryDirectory(prefix='merc-traffic-reset-') as d:
 p=Path(d);c=p/'test.c';exe=p/'test.exe';c.write_text(code)
 subprocess.run(['C:/MinGW/bin/gcc.exe','-std=c11','-O2','-fno-strict-aliasing',str(c),'-o',str(exe)],check=True);subprocess.run([str(exe)],check=True)
 legacy=code.replace('      recomp_traffic_stop_network_totals(esi);','      (void)esi;');assert legacy!=code;c.write_text(legacy)
 subprocess.run(['C:/MinGW/bin/gcc.exe','-std=c11','-O2','-fno-strict-aliasing',str(c),'-o',str(exe)],check=True);assert subprocess.run([str(exe)],capture_output=True).returncode!=0
