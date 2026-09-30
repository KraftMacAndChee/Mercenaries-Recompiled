"""Terminal HQ guard audio waits: no timers, stale VMs, or repeated callbacks."""
from pathlib import Path
import subprocess,tempfile
from test_lua_pool_fallback_native import PRELUDE
ROOT=Path(__file__).resolve().parents[2]
s=(ROOT/'ports/mercenaries/src/recomp_manual.c').read_text()
context=s[s.index('typedef struct recomp_saved_guest_cpu_context'):s.index('#include "voice_callback_queue.h"')]
body=s[s.index('typedef struct recomp_guard_wait'):s.index('/* A failed room script must not leave')]
extra=r"""
static unsigned voice_depth, low_alive, calls;
static uint16_t guest_u16(uint32_t a){return MEM16(a);}
int recomp_xact_low_level_handle_exists(uint32_t h){assert(h==71);return low_alive;}
void sub_00206660(void){abort();}
static void mercenaries_voiceover_stop_callback(void){
 assert(MEM32(g_esp+4)==99 && !guard_wait.handle);
 calls++;assert(recomp_guard_callback_record(99));MEM16(0x371bb0)=0;
 MEM32(0x323acc)=13;g_esp=123;g_eax=456;g_esi=789;
}
static void init(void){
 memset(memory,0,sizeof(memory));memset(&guard_wait,0,sizeof(guard_wait));
 calls=voice_depth=0;low_alive=1;g_esp=0x900000;g_eax=17;g_esi=18;
 MEM16(0x371bb0)=1;MEM32(0x371bb0+0x1a8)=99;MEM32(0x371bb0+0x1ac)=0x371bb8;
 MEM32(0x371bb8)=0x3717a0;strcpy(guest_ptr(0x371bbc),"PreBriefing");
 MEM32(0x3717bc)=0x8cad50;MEM32(0x323acc)=15;
 MEM32(0x413f6c)=1;MEM32(0x413f68)=2;MEM32(0x403970)=3;
 MEM32(0x690ac0)=0x180000;MEM16(0x180000)=1;MEM8(0x180004)=1;
 MEM32(0x180048)=99;MEM32(0x18004c)=71;MEM32(0x18007c)=2;MEM32(0x180090)=0x11ea00;
 voice_depth=1;recomp_guard_request(0x8cad50,99);voice_depth=0;assert(guard_wait.handle==99);
}
int main(void){
 g_xbox_mem_offset=(ptrdiff_t)memory;
 for(unsigned mode=0;mode<6;mode++){
  init();
  if(mode==0)MEM32(0x18007c)=4; /* finished, callback suppressed */
  if(mode==1)MEM8(0x180004)=0; /* already retired */
  if(mode==2)low_alive=0; /* pending low-level stop: let managed update consume it */
  if(mode==3)MEM32(0x18007c)=1; /* queued */
  if(mode==4)MEM32(0x18007c)=0; /* invalid: not proof of completion */
  recomp_guard_poll();assert(calls==(mode<2));
  assert(g_esp==0x900000 && g_eax==17 && g_esi==18);
  recomp_guard_poll();assert(calls==(mode<2));
 }
 for(unsigned mode=0;mode<12;mode++){
  init();MEM32(0x18007c)=4;
  switch(mode){
   case 0:recomp_guard_cancel_owner(0x3717a0);break;
   case 1:MEM32(0x3717bc)++;break;
   case 2:MEM32(0x413f6c)++;break;
   case 3:MEM32(0x413f68)++;break;
   case 4:MEM32(0x403970)++;break;
   case 5:MEM8(0x403384)=1;break;
   case 6:MEM32(0x323acc)=4;break;
   case 7:strcpy(guest_ptr(0x371bbc),"OtherContinuation");break;
   case 8:recomp_guard_error(0x8cad50);break;
   case 9:recomp_guard_callback(0x8cad50);break;
   case 10:MEM32(0x690ac0)=0;break;
   case 11:voice_depth=1;break;
  }
  recomp_guard_poll();assert(!calls);
 }
 init();recomp_guard_call_end();recomp_guard_error(0x8cad50);assert(guard_wait.handle==99);
 init();for(unsigned i=0;i<100000;i++)recomp_guard_poll();assert(!calls&&guard_wait.handle==99);
 init();recomp_guard_cancel_owner(123);recomp_guard_error(123);recomp_guard_callback(123);
 assert(guard_wait.handle==99);MEM32(0x18007c)=4;recomp_guard_poll();assert(calls==1);
 puts("PASS guard wait: terminal-only completion, teardown/errors, live/queued audio, exactly once and CPU preservation");
}
"""
# callback is defined after the production helper, as in the runtime.
code='#define RECOMP_GENERATED_CODE\n#include "'+(ROOT/'ports/mercenaries/src/recomp/recomp_types.h').as_posix()+'"\n'+PRELUDE+'\nstatic unsigned voice_depth;\nstatic uint16_t guest_u16(uint32_t a){return MEM16(a);}\nint recomp_xact_low_level_handle_exists(uint32_t);\n'+context+body+extra.replace('static unsigned voice_depth, low_alive, calls;','static unsigned low_alive, calls;').replace('static uint16_t guest_u16(uint32_t a){return MEM16(a);}','')
with tempfile.TemporaryDirectory(prefix='merc-guard-') as td:
 p=Path(td);(p/'test.c').write_text(code)
 subprocess.run(['C:/MinGW/bin/gcc.exe','-O2','-I',str(ROOT/'ports/mercenaries/src'),str(p/'test.c'),'-o',str(p/'test.exe')],check=True)
 subprocess.run([str(p/'test.exe')],check=True)
