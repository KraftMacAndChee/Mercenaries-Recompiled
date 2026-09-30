"""Execute both generated VO failure branches and their production ABI wrapper."""
from pathlib import Path
import re, subprocess, tempfile, unittest
ROOT=Path(__file__).resolve().parents[2]
class VoiceFailureAbi(unittest.TestCase):
 def test_failure_branches_preserve_guest_context(self):
  s=(ROOT/'ports/mercenaries/src/recomp_manual.c').read_text()
  cpu=s[s.index('typedef struct recomp_saved_guest_cpu_context'):s.index('/* Failure injection is private-harness-only;')]
  g=(ROOT/'ports/mercenaries/src/recomp/gen/recomp_0005.c').read_text()
  branches=g[g.index('loc_0011EB50: ;'):g.index('loc_0011EB66: ;')]
  c=r"""
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
static unsigned char memory[0x4000000];
static uint32_t g_eax,g_ecx,g_edx,g_esp,g_ebx,g_esi,g_edi,g_seh_ebp,g_recomp_current_func,g_fp_top;
static uint16_t g_x87_control_word,g_x87_status_word;
static double g_fp_stack[8];
static float g_xmm0[4],g_xmm1[4],g_xmm2[4],g_xmm3[4],g_xmm4[4],g_xmm5[4],g_xmm6[4],g_xmm7[4];
static uint64_t g_mm0,g_mm1,g_mm2,g_mm3,g_mm4,g_mm5,g_mm6,g_mm7;
#define eax g_eax
#define ecx g_ecx
#define esi g_esi
#define edi g_edi
#define ebx g_ebx
#define esp g_esp
#define MEM32(a) (*(uint32_t*)(memory+(uint32_t)(a)))
#define PUSH32(s,v) do{uint32_t temp=(v);(s)-=4;MEM32(s)=temp;}while(0)
static void *guest_ptr(uint32_t a){return memory+a;}
static uint32_t guest_u32(uint32_t a){return MEM32(a);}
static void xbox_preview_log_event(const char *c,const char *fmt,...){(void)c;(void)fmt;}
void recomp_event_abi_checkpoint(uint32_t a,uint32_t b,uint32_t stack,uint32_t si,uint32_t di){
 assert(esp==stack && esi==si && edi==di);
}
"""+cpu+r"""
static void recomp_guard_cancel_owner(uint32_t owner){(void)owner;}
static void recomp_guard_call_end(void){}
static unsigned calls;
void sub_00113A50(void){
 assert(ecx==0x100000);assert(!strcmp(guest_ptr(MEM32(esp+4)),"PreBriefing"));calls++;
 /* A Lua continuation is free to overwrite all emulated volatile state. */
 g_eax=9;g_ecx=10;g_edx=11;g_esp+=8;g_ebx=12;g_esi=13;g_edi=14;g_seh_ebp=15;
 memset(g_fp_stack,0x99,sizeof(g_fp_stack));g_fp_top=6;g_x87_control_word=99;g_x87_status_word=88;
 memset(g_xmm0,0x88,sizeof(g_xmm0));memset(g_xmm7,0x77,sizeof(g_xmm7));
 g_mm0=123;g_mm7=456;g_recomp_current_func=789;
}
static void branch(int pool_full){
 if(!pool_full)goto loc_0011EB5E;
"""+branches+r"""
loc_0011EB66: ;
}
int main(void){
 unsigned cases=0;
 for(int full=0;full<2;full++)for(int disposition=0;disposition<4;disposition++){
  calls=0;memset(memory,0,sizeof(memory));MEM32(0x10001c)=0x180000;
  strcpy(guest_ptr(0x190000),"PreBriefing");
  eax=edi=0x100000;esi=0x180000;ebx=0x190000;esp=0x300000;
  MEM32(esp+0x10)=0x123456;MEM32(esp)=0x9876;
  uint64_t mark=recomp_voice_call_begin();branch(full);
  assert(!calls && esp==0x300000 && MEM32(esp)==0x9876);
  if(full)assert(edi==0x123456);
  /* Mutating Lua scratch after return must not change the queued name. */
  strcpy(guest_ptr(0x190000),"overwritten");
  if(disposition==1)recomp_voice_cancel_owner(0x100000);
  if(disposition==2)MEM32(0x10001c)=0x180004; /* owner now holds another VM */
  recomp_saved_guest_cpu_context before={0},after={0};
  recomp_save_guest_cpu_context(&before);
  recomp_voice_call_end(mark,disposition!=3);
  recomp_save_guest_cpu_context(&after);
  assert(!memcmp(&before,&after,sizeof(before)));
  assert(calls==(disposition==0));assert(!voice_head && !voice_depth);cases++;
 }
 printf("%u native generated failure branches: stack/register/FP/SIMD restoration and lifecycle cancellation pass\n",cases);
}
"""
  with tempfile.TemporaryDirectory(prefix='merc-voice-abi-') as tmp:
   p=Path(tmp);(p/'test.c').write_text(c)
   subprocess.run(['C:/MinGW/bin/gcc.exe','-std=c11','-O2','-I'+str(ROOT/'ports/mercenaries/src'),str(p/'test.c'),'-o',str(p/'test.exe')],check=True)
   subprocess.run([str(p/'test.exe')],check=True)
if __name__=='__main__':unittest.main()
