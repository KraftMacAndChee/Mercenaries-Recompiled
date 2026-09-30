"""Run generated Lua protected-call/teardown paths, not hand-written scope hooks."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


def function(source, name):
    return re.search(r"void " + name + r"\(void\)\n\{.*?\n\}", source, re.S)[0]


class VoiceCallLifecycle(unittest.TestCase):
    def test_generated_scope_and_teardown(self):
        manual = (ROOT / "ports/mercenaries/src/recomp_manual.c").read_text()
        generated = (ROOT / "ports/mercenaries/src/recomp/gen/recomp_0005.c").read_text()
        cpu = manual[manual.index("typedef struct recomp_saved_guest_cpu_context"):
                     manual.index("/* Failure injection is private-harness-only;")]
        branch = generated[generated.index("loc_0011EB50: ;"):generated.index("loc_0011EB66: ;")]
        wrapper = function(generated, "sub_001135E0")
        teardown = function(generated, "sub_001138F0")
        c = r"""
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
#define edx g_edx
#define esi g_esi
#define edi g_edi
#define ebx g_ebx
#define esp g_esp
#define MEM32(a) (*(uint32_t*)(memory+(uint32_t)(a)))
#define PUSH32(s,v) do{uint32_t temp=(v);(s)-=4;MEM32(s)=temp;}while(0)
#define POP32(s,v) do{(v)=MEM32(s);(s)+=4;}while(0)
#define TEST_Z(a,b) (((a)&(b))==0)
#define RECOMP_TRACE_FUNC(a) ((void)0)
#define RECOMP_ITAIL(a) do { assert((a)==0x1234); esp+=4; } while(0)
static void *guest_ptr(uint32_t a){return memory+a;}
static uint32_t guest_u32(uint32_t a){return MEM32(a);}
static void xbox_preview_log_event(const char *c,const char *fmt,...){(void)c;(void)fmt;}
void recomp_event_abi_checkpoint(uint32_t a,uint32_t b,uint32_t stack,uint32_t si,uint32_t di){
 assert(esp==stack && esi==si && edi==di);
}
void sub_001135E0(void);
void sub_001138F0(void);
void sub_001DD5A0(void);
static unsigned mode, full, calls, errors, ready, nesting, error_cleaned;
void recomp_lua_pcall_checkpoint(uint32_t vm,uint32_t error,uint32_t description){
 assert(vm==0x180000 && description==0x190100);
 if(error) { assert(!error_cleaned); errors++; }
}
void sub_00112120(void){eax=0x100000;esp+=4;}
void sub_00112930(void){esp+=4;}
void sub_001DC980(void){error_cleaned=1;esp+=4;}
void sub_0010FAB0(void){esp+=4;}
void sub_001DDB90(void){esp+=4;}
""" + cpu + r"""
static void recomp_guard_cancel_owner(uint32_t owner){(void)owner;}
static void recomp_guard_call_end(void){}
void sub_00113A50(void){
 assert(ready); /* regression: old generated wrapper invokes before definition */
 assert(ecx==0x100000 && !strcmp(guest_ptr(MEM32(esp+4)),"PreBriefing"));
 calls++;
 g_eax=9;g_ecx=10;g_edx=11;g_esp+=8;g_ebx=12;g_esi=13;g_edi=14;g_seh_ebp=15;
 memset(g_fp_stack,0x99,sizeof(g_fp_stack));g_fp_top=6;g_x87_control_word=99;
 memset(g_xmm7,0x77,sizeof(g_xmm7));g_mm7=456;
}
static void branch(void){
 if(!full)goto loc_0011EB5E;
""" + branch + r"""
loc_0011EB66: ;
}
static void protected_call(void){
 uint32_t stack=esp;
 PUSH32(esp,0x190100);PUSH32(esp,0);PUSH32(esp,0);PUSH32(esp,0x180000);
 PUSH32(esp,0);sub_001135E0();esp+=16;
 assert(esp==stack);
}
void sub_001DD5A0(void){
 recomp_saved_guest_cpu_context saved;
 recomp_save_guest_cpu_context(&saved);
 if(mode==2 && !nesting){
  nesting=1;protected_call();nesting=0;
  assert(!calls); /* nested success must wait for enclosing failure */
 } else {
  eax=edi=0x100000;ebx=0x190000;MEM32(esp+0x10)=0x100000;
  branch();assert(!calls);ready=1;
  if(mode==3){ecx=0x100000;PUSH32(esp,0);sub_001138F0();}
  if(mode==4)MEM32(0x10001c)=0x180004;
 }
 recomp_restore_guest_cpu_context(&saved);
 eax=(mode==1 || (mode==2 && !nesting)) ? 1 : 0;
 esp+=4;
}
""" + wrapper + '\n' + teardown + r"""
int main(void){
 for(full=0;full<2;full++)for(mode=0;mode<5;mode++){
  memset(memory,0,sizeof(memory));calls=errors=ready=nesting=error_cleaned=0;
  MEM32(0x100000)=0x110000;MEM32(0x110010)=0x1234;MEM32(0x10001c)=0x180000;
  strcpy(guest_ptr(0x190000),"PreBriefing");
  esp=0x300000;esi=0x12345;edi=0x23456;ebx=0x34567;g_seh_ebp=0x45678;
  protected_call();
  assert(calls==(mode==0));assert(errors==(mode==1 || mode==2));
  assert(!voice_head && !voice_depth && !voice_draining);
  assert(esi==0x12345 && edi==0x23456 && ebx==0x34567 && g_seh_ebp==0x45678);
 }
 puts("10 generated protected-call cases: ordering, nested failure, teardown, VM replacement and ABI pass");
}
"""
        with tempfile.TemporaryDirectory(prefix="merc-voice-lifecycle-") as temp:
            directory = Path(temp)
            (directory / "test.c").write_text(c)
            subprocess.run(["C:/MinGW/bin/gcc.exe", "-std=c11", "-O2",
                            "-I" + str(ROOT / "ports/mercenaries/src"),
                            str(directory / "test.c"), "-o", str(directory / "test.exe")], check=True)
            subprocess.run([str(directory / "test.exe")], check=True, timeout=15)


if __name__ == "__main__":
    unittest.main()
