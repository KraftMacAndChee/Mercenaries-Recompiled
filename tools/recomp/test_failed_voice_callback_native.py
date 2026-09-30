"""Inject silent cue-start failures into the real managed playback/cleanup code."""
from pathlib import Path
import re, subprocess, tempfile
ROOT=Path(__file__).resolve().parents[2]
s=(ROOT/'ports/mercenaries/src/recomp/gen/recomp_0010.c').read_text(encoding='utf-8')
play=re.search(r'void sub_00206570\(void\)\n\{.*?\n\}',s,re.S)[0]
cleanup=s[s.index('loc_00206522: ;'):s.index('    #undef fp_push',s.index('loc_00206522: ;'))]
prelude=r'''
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
static unsigned char memory[0x900000];
static uint32_t eax,ecx,edx,esi,esp,g_esp_unused;
static float xmm0v[4];
static unsigned banks,sound,callbacks,releases;
#define g_esp esp
#define MEM32(a) (*(uint32_t*)(memory+(uint32_t)(a)))
#define MEM8(a) memory[(uint32_t)(a)]
#define LO8(a) ((uint8_t)(a))
#define PUSH32(s,v) do{uint32_t t=(v);s-=4;MEM32(s)=t;}while(0)
#define POP32(s,v) do{v=MEM32(s);s+=4;}while(0)
#define CMP_EQ(a,b) ((a)==(b))
#define CMP_NE(a,b) ((a)!=(b))
#define CMP_LE(a,b) ((int32_t)(a)<=(int32_t)(b))
#define TEST_Z(a,b) (((a)&(b))==0)
#define TEST_NZ(a,b) (((a)&(b))!=0)
#define RECOMP_TRACE_FUNC(a) ((void)0)
#define recomp_xact_play_checkpoint(a,b,c,d) ((void)0)
static void sub_00206340(void){eax=banks;esp+=4;}
static void sub_00227230(void){eax=sound;esp+=8;}
static void sub_002271A0(void){eax=sound;esp+=20;}
static void icall(uint32_t target){
 if(target==0x11ea00){assert(MEM32(esp+4)==99);assert(!releases);assert(!MEM32(esi+0x4c));callbacks++;esp+=4;}
 else {assert(target==0x12345);releases++;esp+=4;}
}
#define RECOMP_ICALL_SAFE(a,b) icall(a)
'''
main=r'''
int main(void){
 for(banks=0;banks<2;banks++)for(sound=0;sound<2;sound++){
  memset(memory,0,sizeof(memory));MEM32(0x10000+0x34)=0x20000;
  ecx=0x10000;esi=0xabc;esp=0x80000;sub_00206570();
  assert(esp==0x80004 && esi==0xabc);
  unsigned failed=!banks||!sound;
  assert(MEM32(0x10038)==(failed?4:2));
  assert(MEM32(0x10008)==(failed?0:sound));
  /* The caller registers only after PlayManaged returns its nonzero handle.
   * Cleanup must notify before releasing the managed slot, exactly once. */
  MEM32(0x10000)=0x30000;MEM32(0x30004)=0x12345;MEM32(0x10004)=99;
  MEM32(0x1004c)=0x11ea00;callbacks=releases=0;
  esp=0x80000;MEM32(esp)=0xabc;esi=0x10000;eax=MEM32(0x10038);cleanup();
  assert(callbacks==failed && releases==failed && esp==0x8000c && esi==0xabc);
  if(failed){esp=0x80000;MEM32(esp)=0xabc;esi=0x10000;eax=4;cleanup();assert(callbacks==1);}
 }
 /* A real sound explicitly stopped must not fire its cancelled continuation;
  * nor may generic callbacks, empty handles, waiting or playing states. */
 for(unsigned state=0;state<5;state++)for(unsigned h=0;h<2;h++)for(unsigned cb=0;cb<3;cb++){
  memset(memory,0,sizeof(memory));MEM32(0x10000)=0x30000;MEM32(0x30004)=0x12345;
  MEM32(0x10004)=99;MEM32(0x10008)=h;MEM32(0x1004c)=cb==2?0x11ea00:cb;
  callbacks=releases=0;esp=0x80000;MEM32(esp)=0xabc;esi=0x10000;eax=state;cleanup();
  assert(callbacks==(state==4 && h==0 && cb==2));assert(releases==(state==4));
 }
 puts("failed voice: start failure completes once; live/waiting/cancelled cues unchanged");
}
'''
with tempfile.TemporaryDirectory(prefix='merc-failed-voice-') as d:
 p=Path(d);(p/'test.c').write_text(prelude+play+'\nstatic void cleanup(void){\n'+cleanup+'}\n'+main)
 subprocess.run(['C:/MinGW/bin/gcc.exe','-std=c11','-O2',str(p/'test.c'),'-o',str(p/'test.exe')],check=True)
 subprocess.run([str(p/'test.exe')],check=True)
 # The pre-fix finished branch loses the continuation under the same stimulus.
 old=cleanup[:cleanup.index('    /* A managed voice')]+cleanup[cleanup.index('    eax = MEM32(esi);'):]
 (p/'old.c').write_text(prelude+play+'\nstatic void cleanup(void){\n'+old+'}\n'+main)
 subprocess.run(['C:/MinGW/bin/gcc.exe','-std=c11','-O2',str(p/'old.c'),'-o',str(p/'old.exe')],check=True)
 assert subprocess.run([str(p/'old.exe')],capture_output=True).returncode!=0
