"""Exercise missed XACT stops through generated notification and manager cleanup.

Covers completion before registration, the 50-event queue overflowing, ordinary
queued stops, and nonterminal/prepared cues. Active sounds must remain untouched.
"""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
gen11 = (ROOT / 'ports/mercenaries/src/recomp/gen/recomp_0011.c').read_text()
gen12 = (ROOT / 'ports/mercenaries/src/recomp/gen/recomp_0012.c').read_text()
manual = (ROOT / 'ports/mercenaries/src/recomp_manual.c').read_text()

def function(source, name, result='void'):
    return re.search(result + r' ' + name + r'\([^)]*\)\s*\{.*?\n\}', source, re.S)[0]

names = ['sub_0027E350', 'sub_0027E373', 'sub_0027E38F', 'sub_0027E3F8',
         'sub_00283E3E', 'sub_0027E943', 'sub_0027E9C4', 'sub_0027E9F5',
         'sub_0027D8C4', 'sub_00284071']
header = ROOT / 'ports/mercenaries/src/recomp/recomp_types.h'
prelude = '#define RECOMP_GENERATED_CODE\n#include "' + header.as_posix() + '"\n' + r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
uint32_t g_eax,g_ecx,g_edx,g_esp,g_ebx,g_esi,g_edi,g_seh_ebp,g_fp_top;
ptrdiff_t g_xbox_mem_offset; double g_fp_stack[8];
static unsigned char memory[0x4000000];
static unsigned destroyed, fail_allocation;
#define M 0x85c600u
#define P (M+0x7568u)
#define E 0x8c2980u
#define BANK 0x8c3d10u
#define OUTPUT 0x3c0000u
#undef RECOMP_TRACE_FUNC
#define RECOMP_TRACE_FUNC(a) ((void)0)
#undef RECOMP_ICALL_SAFE
#define RECOMP_ICALL_SAFE(a,s) host_call(a)
#define recomp_xact_cue_checkpoint(a,b,c,d) ((void)0)
static void host_call(uint32_t a){assert(a==0x1234);MEM32(MEM32(esp+4))=0;esp+=8;}
static void *guest_ptr(uint32_t a){return memory+a;}
static uint32_t guest_u32(uint32_t a){return MEM32(a);}
static uint16_t guest_u16(uint32_t a){return MEM16(a);}
static uint8_t guest_u8(uint32_t a){return MEM8(a);}
static int xbox_preview_log_enabled(void){return 0;}
#define xbox_preview_log_event(...) ((void)0)
/* All calls in this fixture run under the already-held XACT lock. */
static void sub_0027D5C0(void){eax=0;esp+=4;}
static void sub_0027DB23(void){esp+=4;}
static void sub_0027DB45(void){esp+=4;}
static void sub_0027FB06(void){eax=0;esp+=12;}
static void sub_0027FB39(void){abort();}
static void sub_002821A0(void){abort();}
static void sub_001787C0(void){assert(fail_allocation);eax=0;esp+=12;}
static int recomp_test_fail_guard_notification(uint32_t c){return 0;}
static void sub_0022926D(void){abort();}
static void sub_0027DA34(void){
 assert(MEM32(esp+4)==BANK&&MEM32(esp+8)==0xffffffffu&&MEM32(esp+12)==0);
 uint32_t cue=MEM32(esp+16);assert(MEM8(cue+32)&4);
 assert(!MEM32(cue+40)&&!MEM32(cue+44)&&!MEM32(cue+48));
 destroyed++;eax=0;esp+=20;
}
'''
body = manual[manual.index('typedef struct recomp_xact_failed_subscription'):manual.index('int recomp_test_fail_guard_notification')]
body += function(manual, 'recomp_xact_recover_stop_notification', 'int')
body += '\n' + '\n'.join('void '+n+'(void);' for n in names)
body += '\n' + '\n'.join(function(gen12,n) for n in names)
body += '\n' + function(gen11,'sub_00226510')
body += '\n' + function(gen11,'sub_00226830')
# Execute the actual manager's notification-consumer tail, including callback
# records, original source release, and handle-pool removal.
update = function(gen11,'sub_00226DF0')
body += '\nstatic void drain_manager(void){uint32_t ebp=0;\n'
body += update[update.index('loc_0022706F: ;'):]
main = r'''
static uint32_t cue(unsigned i){return 0x900000+i*0x200;}
static uint32_t wrapper(unsigned i){return P+0x84+i*0x38;}
static void init(unsigned count){
 memset(memory,0,sizeof(memory));memset(xact_failed_subscriptions,0,sizeof(xact_failed_subscriptions));destroyed=fail_allocation=0;g_xbox_mem_offset=(ptrdiff_t)memory;
 MEM32(0x28836c)=E;MEM32(M+4)=E+8;MEM16(P)=count;MEM32(M+0x7564)=64;
 MEM32(0x2dbcf8)=0x1234;
 MEM32(E+0x6c)=MEM32(E+0x70)=E+0x6c;
 MEM32(E+0x74)=MEM32(E+0x78)=E+0x74;
 for(unsigned i=0;i<50;i++){
  ecx=E;esp=0x3e0000;MEM32(esp+4)=0xa00000+i*0x40;sub_0027E373();
 }
 for(unsigned i=0;i<count;i++){
  uint32_t w=wrapper(i),c=cue(i);MEM8(P+4+i)=1;
  MEM32(P+0x1c84+i*8)=i+1;MEM32(P+0x1c88+i*8)=w;
  MEM32(w)=4;MEM32(w+4)=i+1;MEM32(w+8)=c;MEM32(w+12)=i<64?i:0xffffffffu;
  MEM32(w+20)=0x880000;MEM32(0x880008)=BANK;
  MEM32(c+20)=BANK;MEM16(c+24)=i;MEM32(c+36)=c+0x80;
  if(i<64)MEM8(M+0x7524+i)=1;
 }
}
static void register_stop(unsigned i){
 memset(memory+OUTPUT,0,24);MEM32(OUTPUT)=0x40001;MEM32(OUTPUT+4)=BANK;
 MEM32(OUTPUT+12)=cue(i);ecx=cue(i);esp=0x3e0000;
 MEM32(esp+4)=OUTPUT;MEM32(esp+8)=1;sub_00283E3E();assert(esp==0x3e000c);
}
static void notify_stop(unsigned i){
 ecx=cue(i);esp=0x3e0000;MEM32(esp+4)=0;MEM32(esp+8)=1;
 sub_00284071();assert(esp==0x3e000c);
}
static void drain(void){esi=M;esp=0x3e0000;drain_manager();assert(esp==0x3e0080);}
static void verify_all_retired(unsigned count){
 assert(destroyed==count&&MEM16(P)==0&&MEM32(M+0x99f0)==count);
 unsigned seen[128]={0};
 for(unsigned i=0;i<count;i++){
  unsigned h=MEM32(M+0x95f0+i*8);assert(h&&h<=count&&!seen[h-1]);seen[h-1]=1;
  assert(MEM32(M+0x95f4+i*8)==1);assert(!MEM32(cue(i)+0x80+24+12));
 }
 for(unsigned i=0;i<64;i++)assert(!MEM8(M+0x7524+i));
 drain();assert(destroyed==count&&MEM32(M+0x99f0)==0);
}
int main(void){
 /* Play succeeds, but the separate 120-byte STOP descriptor allocation fails.
    The real XACT registration returns OOM; no event will be enqueued at EOF. */
 init(1);MEM32(cue(0)+36)=0;fail_allocation=1;
 register_stop(0);assert(eax==0x8007000eu&&!MEM32(cue(0)+36));
 recomp_xact_stop_subscription(wrapper(0),OUTPUT,eax);
 assert(xact_failed_subscriptions[0].handle==1);
 /* Never finish an active voice, even when its registration failed. */
 MEM8(cue(0)+32)=2;assert(!recomp_xact_recover_stop_notification(E,0,OUTPUT));
 notify_stop(0);assert(MEM32(E+0x6c)==E+0x6c);
 drain();verify_all_retired(1);assert(!xact_failed_subscriptions[0].handle);
 /* Wrapper reuse must forget old subscription intent, including a failed
    registration followed by a successful one and ordinary immediate stop. */
 init(1);register_stop(0);recomp_xact_stop_subscription(wrapper(0),OUTPUT,0x8007000e);
 recomp_xact_stop_subscription(wrapper(0),OUTPUT,0);assert(!xact_failed_subscriptions[0].handle);
 recomp_xact_stop_subscription(wrapper(0),OUTPUT,0x8007000e);
 recomp_xact_forget_subscription(wrapper(0));assert(!xact_failed_subscriptions[0].handle);
 /* A mismatched cue or bank cannot borrow a failed subscription. */
 for(unsigned mode=0;mode<3;mode++){
  init(1);register_stop(0);recomp_xact_stop_subscription(wrapper(0),OUTPUT,0x8007000e);
  MEM32(cue(0)+36)=0;MEM8(cue(0)+32)=6;
  if(mode==0)xact_failed_subscriptions[0].cue+=4;
  if(mode==1)xact_failed_subscriptions[0].handle++;
  if(mode==2)xact_failed_subscriptions[0].descriptor[1]+=4;
  assert(!recomp_xact_recover_stop_notification(E,0,OUTPUT));
 }

 /* A prepared sound can finish before its owner subscribes. Notify will not
    deliver STOP again, because it remembers that STOP already occurred. */
 init(64);
 for(unsigned i=0;i<64;i++){notify_stop(i);register_stop(i);notify_stop(i);}
 assert(MEM32(E+0x6c)==E+0x6c);drain();verify_all_retired(64);
 /* Only 50 of 128 simultaneous completions fit in the retail queue. */
 init(128);
 for(unsigned i=0;i<128;i++){register_stop(i);notify_stop(i);}
 unsigned queued=0;for(uint32_t n=MEM32(E+0x6c);n!=E+0x6c;n=MEM32(n))queued++;
 assert(queued==50);drain();verify_all_retired(128);
 /* Normal queued notifications retain their ordinary order and cleanup. */
 init(32);for(unsigned i=0;i<32;i++){register_stop(i);notify_stop(i);}
 drain();verify_all_retired(32);
 /* Prepared/manual cues, active loops, pending children, absent/mismatched
    registrations, other clients, filters, and full callback buffers are left. */
 for(unsigned mode=0;mode<13;mode++){
  init(1);register_stop(0);MEM8(cue(0)+32)=6;
  uint32_t engine=E,filter=0;
  switch(mode){
  case 0:MEM32(wrapper(0))=1;break;
  case 1:MEM32(wrapper(0))=2;break;
  case 2:MEM8(cue(0)+32)=3;break;
  case 3:case 4:case 5:MEM32(cue(0)+40+(mode-3)*4)=0x910000;break;
  case 6:MEM32(cue(0)+0x80+24+12)=0;break;
  case 7:MEM32(cue(0)+0x80+24+4)=BANK+4;break;
  case 8:MEM32(cue(0)+0x80+24)=0x80040001;break;
  case 9:engine+=8;break;
  case 10:filter=OUTPUT;break;
  case 11:MEM32(M+0x99f0)=128;break;
  case 12:MEM32(wrapper(0)+4)=999;break;
  }
  assert(!recomp_xact_recover_stop_notification(engine,filter,OUTPUT));
  assert(MEM16(P)==1&&MEM8(M+0x7524)==1&&!destroyed);
 }
 puts("PASS: actual registration OOM, active sound preserved, wrapper reuse/identity guards, early completion, 128-stop burst, exactly-once cleanup, negative control");
}
'''
code = prelude + body + main
with tempfile.TemporaryDirectory(prefix='merc-xact-stop-') as directory:
    directory = Path(directory)
    source = directory / 'test.c'
    source.write_text(code)
    exe = directory / 'test.exe'
    subprocess.run(['C:/MinGW/bin/gcc.exe','-std=c11','-O2','-fno-strict-aliasing','-I'+str(ROOT/'ports/mercenaries/src'),str(source),'-o',str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
    legacy = code.replace('if (recomp_xact_recover_stop_notification(MEM32(ebp + 8),\n              MEM32(ebp + 12), MEM32(ebp + 16))) {','if (0) {')
    assert legacy != code
    source.write_text(legacy)
    subprocess.run(['C:/MinGW/bin/gcc.exe','-std=c11','-O2','-fno-strict-aliasing','-I'+str(ROOT/'ports/mercenaries/src'),str(source),'-o',str(exe)], check=True)
    assert subprocess.run([str(exe)],capture_output=True).returncode != 0
