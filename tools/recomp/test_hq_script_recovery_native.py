"""Exercise HQ error recovery policy and its guest call boundary."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / 'ports/mercenaries/src/recomp_manual.c').read_text()
recovery = source[source.index('static uint32_t hq_error_owner'):source.index('/* Guard-side state was absent')]
prelude = r'''
#include <stdint.h>
static void recomp_guard_poll(void) {}
#include <string.h>
#include <stdio.h>
#include <assert.h>
static unsigned char memory[0x500000];
#define MEM32(a) (*(uint32_t*)(memory+(a)))
static uint32_t g_eax,g_ecx,g_esp,voice_depth;
static unsigned calls,accepted,cancelled;
static void *guest_ptr(uint32_t a){return memory+a;}
static uint32_t guest_u32(uint32_t a){return MEM32(a);}
static uint8_t guest_u8(uint32_t a){return memory[a];}
static void recomp_guest_push_u32(uint32_t v){g_esp-=4;MEM32(g_esp)=v;}
typedef struct {uint32_t a,c,s;} recomp_saved_guest_cpu_context;
static void recomp_save_guest_cpu_context(recomp_saved_guest_cpu_context *s){s->a=g_eax;s->c=g_ecx;s->s=g_esp;}
static void recomp_restore_guest_cpu_context(const recomp_saved_guest_cpu_context *s){g_eax=s->a;g_ecx=s->c;g_esp=s->s;}
static void xbox_preview_log_event(const char *a,const char*b,...){(void)a;(void)b;}
void sub_00113A50(void);
void sub_0010FAB0(void){assert(MEM32(g_esp+4)==0x110000);cancelled++;g_esp+=4;}
void sub_00121BD0(void){assert(g_ecx==0x414150 && MEM32(g_esp+4)==0x4c713ab3);g_eax=accepted;g_esp+=8;}
'''
main = r'''
void sub_00113A50(void){
 assert(cancelled==1 && g_ecx==0x100000);
 assert(!strcmp(guest_ptr(MEM32(g_esp+4)),calls ? "onUseExit" : "ThreeStageInteractionCleanup"));
 calls++;recomp_hq_note_script_error(0x110000); /* exit failure must not loop */
 g_eax=1;g_ecx=2;g_esp+=8;
}
static void reset(void){
 memset(memory,0,sizeof(memory));hq_error_owner=0;calls=accepted=voice_depth=cancelled=0;
 g_eax=123;g_ecx=456;g_esp=0x300000;
 MEM32(0x403378)=0x100000;MEM32(0x10001c)=0x110000;MEM32(0x100028)=0x21c08419;
 MEM32(0x403370)=0x9988;memory[0x403384]=memory[0x403385]=memory[0x403430]=1;
 memory[0x4034ac]=255;strcpy(guest_ptr(0x403644),"EnterBriefing");
}
int main(void){
 reset();recomp_hq_recover_script_error();assert(!calls);
 /* Non-HQ scripts, wrong VM, missing scene and other fades never arm recovery. */
 for(int i=0;i<7;i++){
  reset();switch(i){
   case 0:MEM32(0x100028)=0x1234;break;
   case 1:MEM32(0x10001c)++;break;
   case 2:memory[0x403384]=0;break;
   case 3:MEM32(0x403370)=0;break;
   case 4:memory[0x403430]=0;break;
   case 5:memory[0x4034ac]=254;break;
   case 6:strcpy(guest_ptr(0x403644),"Exit2");break;
  }
  recomp_hq_note_script_error(0x110000);assert(!hq_error_owner);
 }
 reset();recomp_hq_note_script_error(0x110000);
 memory[0x403385]=0;recomp_hq_recover_script_error();assert(!calls&&hq_error_owner);
 memory[0x403385]=1;voice_depth=1;recomp_hq_recover_script_error();assert(!calls&&hq_error_owner);
 voice_depth=0;recomp_hq_recover_script_error();recomp_hq_recover_script_error();
 assert(calls==2&&cancelled==1&&!hq_error_owner&&!hq_recovering);
 assert(g_eax==123&&g_ecx==456&&g_esp==0x300000);
 /* Discard stale failures rather than applying them to a different entry. */
 for(int i=0;i<4;i++){
  reset();recomp_hq_note_script_error(0x110000);
  if(i==0)MEM32(0x10001c)++;if(i==1)MEM32(0x403370)++;
  if(i==2)memory[0x403384]=0;if(i==3)strcpy(guest_ptr(0x403644),"DoNothing");
  recomp_hq_recover_script_error();assert(!calls&&!hq_error_owner);
 }
 reset();recomp_hq_note_script_error(0x110000);accepted=1;
 recomp_hq_recover_script_error();assert(!calls&&!hq_error_owner);
 assert(g_eax==123&&g_ecx==456&&g_esp==0x300000);
 puts("HQ recovery: normal entry untouched, 12 exclusion cases, deferred exit, exactly-once dispatch and guest ABI pass");
}
'''
with tempfile.TemporaryDirectory(prefix='merc-hq-recovery-') as tmp:
    path = Path(tmp)
    (path / 'test.c').write_text(prelude + recovery + main)
    subprocess.run(['C:/MinGW/bin/gcc.exe', '-std=c11', '-O2', str(path/'test.c'), '-o', str(path/'test.exe')], check=True)
    subprocess.run([str(path/'test.exe')], check=True, timeout=15)
