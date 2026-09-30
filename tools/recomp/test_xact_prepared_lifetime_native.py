"""Retired prepared cues must not pass reused memory to XACT Play.

Runs the production destructor hook, manager Play, XACT Play, source attachment,
and source AddRef. Models reuse with the exact bad sound pointer from dump 22972.
"""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
gen11 = (ROOT / 'ports/mercenaries/src/recomp/gen/recomp_0011.c').read_text(encoding='utf-8')
gen12 = (ROOT / 'ports/mercenaries/src/recomp/gen/recomp_0012.c').read_text(encoding='utf-8')
manual = (ROOT / 'ports/mercenaries/src/recomp_manual.c').read_text(encoding='utf-8')
def function(source, name):
    return re.search(r'void ' + name + r'\([^)]*\)\s*\{.*?\n\}', source, re.S)[0]
header = ROOT / 'ports/mercenaries/src/recomp/recomp_types.h'
prelude = '#define RECOMP_GENERATED_CODE\n#include "' + header.as_posix() + '"\n' + r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
uint32_t g_eax,g_ecx,g_edx,g_esp,g_ebx,g_esi,g_edi,g_seh_ebp,g_fp_top;
ptrdiff_t g_xbox_mem_offset; double g_fp_stack[8];
static unsigned char memory[0x4000000];
#define M 0x85c600u
#define W (M+0x7568u+0x84u)
#define ENTRY (M+0x91ecu)
#define CUE 0x2000000u
#define SOUND 0x2001000u
#define DATA 0x2002000u
#define BANK 0x8c3d10u
#define SOURCE 0x8c5880u
#define STACK 0x8bf900u
#undef RECOMP_TRACE_FUNC
#define RECOMP_TRACE_FUNC(a) ((void)0)
#undef RECOMP_ICALL_SAFE
#define RECOMP_ICALL_SAFE(a,s) do { assert(!"unexpected lock"); } while(0)
#undef MEM8
static uint8_t *byte_at(uint32_t a) {
 if(a>=sizeof(memory)) { fputs("stale sound read\n",stderr);exit(91); }
 return memory+a;
}
#define MEM8(a) (*byte_at((uint32_t)(a)))
#define recomp_xact_cue_checkpoint(a,b,c,d) ((void)0)
#define recomp_xact_cue_lifetime_checkpoint(a,b,c) ((void)0)
#define recomp_xact_alloc_checkpoint(a,b,c,d) ((void)0)
#define xbox_preview_log_event(...) ((void)0)
static void *guest_ptr(uint32_t a){return memory+a;}
static uint32_t guest_u32(uint32_t a){return MEM32(a);}
static unsigned fresh,played,subscribed,released,fail,drain;
void recomp_xact_forget_subscription(uint32_t w){}
void recomp_xact_stop_subscription(uint32_t w,uint32_t d,uint32_t hr){assert(hr==0);}
static void sub_0027D5C0(void){eax=0;esp+=4;}
static void sub_002845D5(void){assert(ecx==CUE);esp+=4;}
static void sub_001787D0(void){
 assert(MEM32(esp+4)==CUE);memset(memory+CUE,0,64);
 MEM32(CUE+0x2c)=0x11229;MEM32(0x11239)=0xd9c1decc;esp+=12;
}
static void sub_0027F90A(void){assert(ecx==BANK);eax=0;esp+=8;}
static void sub_00280B15(void){abort();}
static void sub_0027F055(void){abort();}
static void sub_0028047A(void){
 fresh++;assert(ecx==BANK);assert((MEM32(MEM32(esp+4))&4)==0);
 if(fail){eax=0x8007000e;esp+=16;return;}
 memset(memory+CUE,0,64);MEM32(CUE+0x28)=SOUND;MEM32(SOUND+0x10)=DATA;
 MEM32(MEM32(esp+12))=CUE;eax=0;esp+=16;
}
static void sub_0028426D(void){MEM32(MEM32(esp+4))=0;esp+=8;}
static void sub_00284179(void){esp+=8;}
static void sub_002841B2(void){esp+=8;}
static void sub_002842E0(void){abort();}
static void sub_00284849(void){played++;eax=0;esp+=4;}
static void sub_0028504F(void){abort();}
void sub_00284650(void);
static void sub_0027F3AA(void){
 if(drain){PUSH32(esp,1);ecx=CUE;PUSH32(esp,0);sub_00284650();}
 esp+=4;
}
static void sub_00225570(void){esp+=8;}
static void sub_0027D84E(void){subscribed++;eax=0;esp+=12;}
static void sub_00226510(void){released++;assert(MEM32(esp+4)==123);esp+=8;}
static void sub_002816B8(void){abort();}
/* Passive IRQL means the RAII lock argument is cleared; no guest registers change. */
static void sub_002807FA(void){MEM32(ecx)=0;esp+=8;}
'''
names12=['sub_00284650','sub_00280C0A','sub_00285366','sub_00285401',
         'sub_00283F45','sub_002805B7','sub_0027DA01']
code=prelude+function(manual,'recomp_xact_retire_prepared_cue')+'\n'
code+='\n'.join(function(gen12,n) for n in names12)
code+=function(gen11,'sub_00226830')+function(gen11,'sub_00226C70')
code+=r'''
static void setup(unsigned state){
 memset(memory,0,sizeof(memory));fresh=played=subscribed=released=fail=drain=0;
 MEM32(ENTRY)=123;MEM32(ENTRY+4)=W;
 MEM32(W)=state;MEM32(W+4)=123;MEM32(W+8)=CUE;MEM32(W+12)=0;
 MEM32(W+20)=0x300000;MEM32(0x300008)=BANK;MEM32(W+24)=0x115;
 MEM32(M+0x7424)=SOURCE;MEM8(M+0x7524)=1;MEM32(M+0x7564)=1;
 MEM32(CUE+0x28)=SOUND;MEM32(SOUND+0x10)=DATA;
 eax=ecx=edx=0;ebx=0xabc;esi=0xdef;edi=0x123;g_seh_ebp=0x456;
}
static void destroy(void){
 esp=STACK;ecx=CUE;MEM32(esp+4)=1;sub_00284650();
 assert(esp==STACK+8&&esi==0xdef&&ebx==0xabc&&edi==0x123);
}
static void play(void){
 esp=STACK;ecx=M;MEM32(esp+4)=W;MEM32(esp+8)=0x300100;
 sub_00226C70();assert(esp==STACK+12&&esi==0xdef&&ebx==0xabc&&edi==0x123);
}
int main(void){
 g_xbox_mem_offset=(ptrdiff_t)memory;
 /* Run the stale/reuse path first so the old control reaches the invalid read. */
 for(unsigned state=1;state<=3;state++){
  setup(state);MEM32(W+44)=0x3f800000;MEM32(W+48)=state==2;
  destroy();assert(MEM32(W)==state&&MEM32(W+44)==0x3f800000&&MEM32(W+48)==(state==2));
  play();assert(fresh==1&&played==1&&subscribed==1&&released==0);
  assert(MEM32(W)==4&&MEM32(W+8)==CUE&&MEM32(SOURCE+8)==1);
  setup(state);play();assert(fresh==0&&played==1&&subscribed==1);
  /* Destruction can happen inside Play while DoWork drains its 50-event queue. */
  setup(state);drain=1;MEM32(M+0x9a00)=50;play();
  assert(fresh==1&&played==1&&subscribed==1);
  /* Replacement allocation failure takes normal cleanup, never plays garbage. */
  setup(state);destroy();fail=1;play();
  assert(fresh==1&&!played&&!subscribed&&released==1&&eax==0);
  assert(MEM32(W)==0&&!MEM8(M+0x7524)&&!MEM32(M+0x7564));
 }
 /* Do not steal completion identities from playing/paused/finished wrappers. */
 for(unsigned state=0;state<=6;state++){
  setup(state);destroy();assert(MEM32(W+8)==((state>=1&&state<=3)?0:CUE));
 }
 setup(1);MEM32(ENTRY)=0;destroy();assert(MEM32(W+8)==CUE);
 setup(1);MEM32(ENTRY)=124;destroy();assert(MEM32(W+8)==CUE);
 setup(1);MEM32(ENTRY+4)=0xd9c1decc;destroy();assert(MEM32(W+8)==CUE);
 setup(1);MEM32(W+8)=CUE+64;destroy();assert(MEM32(W+8)==CUE+64);
 /* Repeated address and wrapper reuse stays bounded, without leaking sources. */
 for(unsigned i=0;i<512;i++){setup(1);destroy();play();assert(fresh==1&&played==1);}
 puts("Prepared XACT lifetime: stale reuse, healthy playback, cleanup, state/identity and 512 reuse cycles passed");
}
'''
with tempfile.TemporaryDirectory(prefix='merc-xact-lifetime-') as d:
    p=Path(d)
    for label, source in [('fixed',code),('old',code.replace('recomp_xact_retire_prepared_cue(esi);','/* old: retains retired pointer */')),('stale-flags',code.replace('    ebx = MEM32(esi + 8u) ? 4u : 0u; /* DoWork may retire the prepared cue */',''))]:
        assert label=='fixed' or source!=code
        (p/(label+'.c')).write_text(source,encoding='utf-8')
        subprocess.run(['C:/MinGW/bin/gcc.exe','-std=c11','-O2','-fno-strict-aliasing','-I'+str(ROOT/'ports/mercenaries/src'),str(p/(label+'.c')),'-o',str(p/(label+'.exe'))],check=True)
        r=subprocess.run([str(p/(label+'.exe'))],capture_output=True,text=True)
        if label=='fixed':
            assert r.returncode==0,(r.returncode,r.stdout,r.stderr)
            print(r.stdout.strip())
        elif label=='stale-flags':
            assert r.returncode!=0,'Cached PREPARED flags escaped the queue-drain test'
            print('Queue-drain negative control fails without recalculating PREPARED flags')
        else:
            assert r.returncode==91 and 'stale sound read' in r.stderr,(r.returncode,r.stderr)
            print('Pre-fix negative control reaches the dump-matching invalid sound read')
