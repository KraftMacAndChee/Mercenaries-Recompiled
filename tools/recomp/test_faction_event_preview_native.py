"""Run the real bounded observer and regenerated retail cancellation routine."""
from pathlib import Path
import re,subprocess,tempfile
ROOT=Path(__file__).resolve().parents[2]
body=None
for p in (ROOT/'ports/mercenaries/src/recomp/gen').glob('recomp_*.c'):
 if p.name=='recomp_dispatch.c':continue
 m=re.search(r'void sub_0010FA80\(void\)\n\{.*?\n\}',p.read_text(),re.S)
 if m:body=m[0];break
assert body and 'recomp_preview_faction_event(2u, eax)' in body
code='#define RECOMP_GENERATED_CODE\n#include "'+(ROOT/'ports/mercenaries/src/recomp/recomp_types.h').as_posix()+'"\n'+r'''
#include <assert.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
uint32_t g_eax,g_ecx,g_edx,g_esp,g_ebx,g_esi,g_edi,g_seh_ebp,g_fp_top;
ptrdiff_t g_xbox_mem_offset;
double g_fp_stack[8];
float g_xmm0[4],g_xmm1[4],g_xmm2[4],g_xmm3[4],g_xmm4[4],g_xmm5[4],g_xmm6[4],g_xmm7[4];
static unsigned char memory[0x400000],before[0x400000];
#undef RECOMP_TRACE_FUNC
#define RECOMP_TRACE_FUNC(a) ((void)0)
typedef unsigned long long ULONGLONG;
#define __declspec(x)
static ULONGLONG clock_ms;
static unsigned enabled=1,reads,logs;
static char line[1536];
static int xbox_preview_log_enabled(void){return enabled;}
static ULONGLONG GetTickCount64(void){return clock_ms;}
static uint32_t guest_u32(uint32_t p){assert(p<=sizeof(memory)-4);reads++;return MEM32(p);}
static float guest_f32(uint32_t p){assert(p<=sizeof(memory)-4);reads++;return MEMF(p);}
#define xbox_preview_log_sample xbox_preview_log_event
static void xbox_preview_log_event(const char*category,const char*fmt,...){
 assert(!strcmp(category,"faction-event"));va_list ap;va_start(ap,fmt);int n=vsnprintf(line,sizeof(line),fmt,ap);va_end(ap);assert(n>0&&n<sizeof(line));logs++;
}
'''+ '#include "'+(ROOT/'ports/mercenaries/src/faction_preview.h').as_posix()+'"\n'+body+r'''
#define EVENT 0x10000u
static void observe(unsigned phase,unsigned address){
 memcpy(before,memory,sizeof(memory));
 eax=0x11223344;ecx=0x22334455;edx=0x33445566;esp=0x3E0000;ebx=77;esi=88;edi=99;g_seh_ebp=111;g_fp_top=4;g_fp_stack[4]=1.234;
 recomp_preview_faction_event(phase,address);
 assert(!memcmp(before,memory,sizeof(memory)));
 assert(eax==0x11223344&&ecx==0x22334455&&edx==0x33445566&&esp==0x3E0000&&ebx==77&&esi==88&&edi==99&&g_seh_ebp==111&&g_fp_top==4&&g_fp_stack[4]==1.234);
}
int main(void){
 g_xbox_mem_offset=(ptrdiff_t)memory;
 MEM32(EVENT+4)=0x123456;MEM32(EVENT+0x14)=26;MEM32(EVENT+0xA8)=42;
 strcpy((char*)memory+EVENT+0x38,"FactionAttitudeChange");
 MEM32(0x30E7A0)=150;MEM32(0x30E7B0)=125;MEM32(0x365EB4)=170;MEMF(0x3233F8)=-1;
 enabled=0;observe(1,EVENT);assert(!logs&&!reads);enabled=1;
 observe(0,EVENT);observe(4,EVENT);observe(1,0xFFFFFFFF);observe(1,0);assert(!logs&&!reads);
 const char*names[]={"BouncerUsed","BouncerBribeRequested","FactionAttitudeChange"};
 for(unsigned j=0;j<3;j++)for(unsigned phase=1;phase<=3;phase++){
  strcpy((char*)memory+EVENT+0x38,names[j]);clock_ms+=1000;unsigned prev=logs;observe(phase,EVENT);assert(logs==prev+1&&strstr(line,names[j])&&strstr(line,"active=150 free=125 high_water=170"));
 }
 unsigned prev=logs;strcpy((char*)memory+EVENT+0x38,"BouncerMoodReaction");observe(1,EVENT);assert(logs==prev);
 memset(memory+EVENT+0x38,'B',96);observe(1,EVENT);assert(logs==prev);
 memcpy(memory+EVENT+0x38,"Bouncer",7);observe(1,EVENT);assert(logs==prev);
 strcpy((char*)memory+EVENT+0x38,"BouncerUsed");clock_ms+=1000;prev=logs;
 for(unsigned i=0;i<100000;i++)recomp_preview_faction_event(1,EVENT);
 assert(logs==prev+32);clock_ms+=1000;observe(2,EVENT);assert(strstr(line,"dropped=99968"));
 /* Retail cancellation must affect only the first matching live event ID. */
 for(unsigned trial=0;trial<300;trial++){
  memset(memory,0,sizeof(memory));
  for(unsigned i=0;i<275;i++){
   unsigned event=0x100000+i*256,node=event+8;
   MEM32(node)=i==274?0x30E798:node+256;MEM32(node+8)=event;
   MEM32(event+0xA8)=i+1;strcpy((char*)memory+event+0x38,i%2?"BouncerUsed":"OtherTimer");
  }
  MEM32(0x30E794)=0x100008;MEM32(0x30E798+8)=0;
  esp=0x3E0000;MEM32(esp+4)=trial;esi=99;edi=77;ebx=66;g_seh_ebp=55;g_fp_top=0;
  sub_0010FA80();assert(esp==0x3E0004&&esi==99&&edi==77&&ebx==66&&g_seh_ebp==55&&g_fp_top==0);
  for(unsigned i=0;i<275;i++)assert(MEM32(0x100000+i*256+0xA8)==(trial==i+1?0xFFFFFFFFu:i+1));
 }
 puts("PASS: bounded named-event records; disabled/invalid/unterminated rejection; read-only memory/registers; 100000-call rate limit; 300 retail cancellation cases across a full 275-event pool");
}
'''
with tempfile.TemporaryDirectory(prefix='faction-events-') as t:
 p=Path(t);(p/'fixture.c').write_text(code)
 subprocess.run(['C:/MinGW/bin/gcc.exe','-O1','-fno-strict-aliasing',str(p/'fixture.c'),'-o',str(p/'check.exe')],check=True)
 subprocess.run([str(p/'check.exe')],check=True)
