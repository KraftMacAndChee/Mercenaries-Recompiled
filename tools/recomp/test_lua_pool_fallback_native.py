"""Replay Lua table-growth OOM through actual lifted Lua/RedMemory routines.

Optional --snapshot uses a private 64MiB AN-HQ failure capture, never a process.
The legacy build reproduces ActorInit=false; the candidate grows the table and
preserves all existing bindings. Synthetic tests additionally cover allocator
ownership, both realloc directions, failure retention and complete reclamation.
"""
from pathlib import Path
import argparse, re, subprocess, tempfile
ROOT = Path(__file__).resolve().parents[2]

PRELUDE = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
uint32_t g_eax,g_ecx,g_edx,g_esp,g_ebx,g_esi,g_edi,g_seh_ebp,g_fp_top;
volatile uint32_t g_recomp_current_func;
ptrdiff_t g_xbox_mem_offset;
double g_fp_stack[8];
uint16_t g_x87_control_word,g_x87_status_word;
float g_xmm0[4],g_xmm1[4],g_xmm2[4],g_xmm3[4],g_xmm4[4],g_xmm5[4],g_xmm6[4],g_xmm7[4];
uint64_t g_mm0,g_mm1,g_mm2,g_mm3,g_mm4,g_mm5,g_mm6,g_mm7;
static unsigned char memory[0x4000000];
static int ordinary_enabled, ordinary_frees, fallback_events;
static uint32_t ordinary_address=0xA00000, g_merc_heap_initialized=1, g_merc_heap_handle=1, g_merc_heap_block_count;
static jmp_buf error_return;
#undef RECOMP_ITAIL
#define RECOMP_ITAIL(a) abort()
#undef RECOMP_ICALL_SAFE
#define RECOMP_ICALL_SAFE(a,b) abort()
#undef RECOMP_TRACE_FUNC
#define RECOMP_TRACE_FUNC(a) ((void)0)
static void *guest_ptr(uint32_t a){return memory+a;}
static uint32_t guest_u32(uint32_t a){return MEM32(a);}
static uint8_t guest_u8(uint32_t a){return MEM8(a);}
static uint32_t merc_heap_allocate(uint32_t h,uint32_t f,uint32_t n){return ordinary_enabled ? ordinary_address : 0;}
static uint32_t merc_heap_free(uint32_t a){ordinary_frees++;return 1;}
static void xbox_preview_log_event(const char *a,const char *b,...){fallback_events++;}
void sub_001F6C20(void){fputs("unexpected metadata exhaustion\n",stderr);abort();}
void sub_001DE540(void){fputs("unexpected Lua non-memory error\n",stderr);abort();}
void sub_001DE930(void){assert(MEM32(esp+8)==4);longjmp(error_return,1);}
void sub_002375B4(void){abort();}
'''

HARNESS = r'''
enum {POOL=0x645984, PTABLE=0x700000, L=0x740000, G=0x741000, T=0x720000, N=0xA00000, KEY=0x730000, STR=0x710000, STACK=0x87D000};
static void cpu(void){esp=STACK;eax=11;ecx=22;edx=33;ebx=44;esi=55;edi=66;g_seh_ebp=77;}
static void init(void){
 memset(memory,0,sizeof(memory));cpu();ecx=POOL;
 MEM32(esp+4)=0x1000000;MEM32(esp+8)=0x1000000;MEM32(esp+12)=PTABLE;MEM32(esp+16)=7000;MEM32(esp+20)=64;sub_001F6710();
 MEM32(0x1e2b80+3*4)=0x1e2af1; /* retail string-key hash dispatch */
 for(unsigned i=1;i<256;i++){unsigned v=i,n=0;while(v>>=1)n++;MEM8(0x2fbd87+i)=n;}
}
static uint32_t change(uint32_t p,uint32_t n,uint32_t old){
 cpu();MEM32(esp+4)=p;MEM32(esp+8)=n;MEM32(esp+12)=old;
 sub_00178870();assert(esp==STACK+4&&ecx==22&&edx==33&&ebx==44&&esi==55&&edi==66&&g_seh_ebp==77);return eax;
}
static void table_test(const char *snapshot){
 uint32_t vm=L,t=T,nodes=N,keystr,slot;
 if(snapshot){
  FILE *f=fopen(snapshot,"rb");assert(f);assert(fread(memory,1,sizeof(memory),f)==sizeof(memory));fclose(f);
  vm=MEM32(MEM32(0x403378)+0x1c);t=MEM32(vm+0x48);nodes=MEM32(t+16);assert(MEM8(t+7)==9);
  slot=0;for(unsigned i=0;i<512;i++){uint32_t x=nodes+i*40;assert(MEM32(x)==4);if(!strcmp((char*)guest_ptr(MEM32(x+8)+16),"ActorInit"))slot=x;}
  assert(slot&&MEM32(slot+16)==1&&MEM32(slot+24)==0&&MEM32(slot+32)==0);keystr=MEM32(slot+8);
  MEM32(slot)=MEM32(slot+16)=0;MEM32(t+20)=slot;
 }else{
  MEM32(vm+16)=G;MEM32(G+36)=20480;MEM8(t+4)=5;MEM8(t+7)=9;MEM32(t+16)=nodes;MEM32(t+20)=nodes+511*40;
  for(unsigned i=0;i<512;i++){
   uint32_t s=STR+i*32;MEM8(s+4)=4;MEM32(s+8)=i;MEM32(s+12)=sprintf((char*)guest_ptr(s+16),"key%u",i);
   if(i<511){MEM32(nodes+i*40)=4;MEM32(nodes+i*40+8)=s;MEM32(nodes+i*40+16)=1;MEM32(nodes+i*40+24)=1;}
  }
  keystr=STR+511*32;slot=nodes+511*40;
 }
 /* Snapshot globals and main-pool contents are untouched except for restoring
  * the final insertion to its immediate pre-failure state above. */
 unsigned char before[512*40];memcpy(before,guest_ptr(nodes),sizeof(before));
 uint32_t free_before=MEM32(POOL+8);MEM32(KEY)=4;MEM32(KEY+8)=keystr;
 cpu();MEM32(esp+4)=vm;MEM32(esp+8)=t;MEM32(esp+12)=KEY;
 int failed=setjmp(error_return);
 if(!failed)sub_001E3520();
#ifdef LEGACY
 assert(failed&&MEM8(t+7)==9&&MEM32(slot+16)==1&&MEM32(slot+24)==0);
 assert(MEM32(POOL+8)==free_before);puts("PASS legacy: table growth OOM leaves the captured ActorInit=false marker");
#else
 assert(!failed&&esp==STACK+4&&MEM8(t+7)==10&&MEM32(eax)==0&&g_merc_lua_pool_blocks);
 MEM32(eax)=6;MEM32(eax+8)=0x123456; /* completed caller assignment */
 uint32_t replacement=MEM32(t+16);assert(replacement==g_merc_lua_pool_blocks->address);
 for(unsigned i=0;i<512;i++){
  uint32_t oldkey;memcpy(&oldkey,before+i*40,4);if(!oldkey)continue;
  uint32_t s;memcpy(&s,before+i*40+8,4);unsigned found=0;
  for(unsigned j=0;j<1024;j++){uint32_t nn=replacement+j*40;if(MEM32(nn)==4&&MEM32(nn+8)==s){assert(!memcmp(guest_ptr(nn+16),before+i*40+16,4));assert(!memcmp(guest_ptr(nn+24),before+i*40+24,8));found++;}}
  assert(found==1);
 }
 merc_lua_free(replacement);assert(!g_merc_lua_pool_blocks&&MEM32(POOL+8)==free_before);
 puts("PASS candidate: 512->1024 table growth, all 511 existing globals intact, memory returned");
#endif
}
int main(int argc,char **argv){
 g_xbox_mem_offset=(ptrdiff_t)memory;init();table_test(argc>1?argv[1]:NULL);
#ifndef LEGACY
 init();uint32_t free_before=MEM32(POOL+8);ordinary_enabled=1;
 uint32_t p=change(0,4096,0);assert(p==ordinary_address&&!g_merc_lua_pool_blocks);memset(guest_ptr(p),0xA5,4096);
 ordinary_enabled=0;uint32_t q=change(p,40960,4096);assert(q&&q!=p&&g_merc_lua_pool_blocks);for(unsigned i=0;i<4096;i++)assert(MEM8(q+i)==0xA5);
 assert(change(q,0x4000001,40960)==0&&g_merc_lua_pool_blocks);for(unsigned i=0;i<4096;i++)assert(MEM8(q+i)==0xA5);
 uint32_t old_q=q;q=change(q,81920,40960);assert(q&&q!=old_q&&g_merc_lua_pool_blocks&&!g_merc_lua_pool_blocks->next);for(unsigned i=0;i<4096;i++)assert(MEM8(q+i)==0xA5);
 ordinary_enabled=1;p=change(q,2048,81920);assert(p==ordinary_address&&!g_merc_lua_pool_blocks&&MEM32(POOL+8)==free_before);for(unsigned i=0;i<2048;i++)assert(MEM8(p+i)==0xA5);
 change(p,0,2048);ordinary_enabled=0;
 for(unsigned i=0;i<1000;i++){p=change(0,1+i%101,0);assert(p&&g_merc_lua_pool_blocks);change(p,0,1+i%101);assert(!g_merc_lua_pool_blocks&&MEM32(POOL+8)==free_before);}
 uint32_t a=change(0,4*1024*1024,0),b=change(0,4*1024*1024,0),c=change(0,4*1024*1024,0);assert(a&&b&&c);
 change(a,0,4*1024*1024);change(c,0,4*1024*1024);assert(MEM32(POOL+8)==12*1024*1024);
 assert(!change(0,10*1024*1024,0)&&g_merc_lua_pool_blocks&&!g_merc_lua_pool_blocks->next);change(b,0,4*1024*1024);assert(MEM32(POOL+8)==free_before&&!g_merc_lua_pool_blocks);
 assert(change(0,0,0)==1);assert(change(1,0,0)==0);
 MEM32(POOL+0x1c)=MEM32(POOL+0x18)+2;assert(!change(0,4096,0));MEM32(POOL)=0;assert(!change(0,4096,0));
 puts("PASS ownership, realloc preservation, failure retention, early/metadata guards, 1000 reclamations and guest ABI");
#endif
}
'''

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--snapshot',type=Path);parser.add_argument('--msvc',action='store_true');args=parser.parse_args()
    source='\n'.join(p.read_text() for p in (ROOT/'ports/mercenaries/src/recomp/gen').glob('recomp_*.c'))
    funcs={n:t for t,n in re.findall(r'(void (sub_[0-9A-F]{8})\(void\)\n\{.*?\n\})',source,re.S)}
    selected=set();stop={'sub_001F6C20','sub_001DE540','sub_001DE930','sub_002375B4','sub_00178870'}
    def add(n):
        if n in selected or n in stop:return
        selected.add(n)
        for child in re.findall(r'(sub_[0-9A-F]{8})\(\);',funcs[n]):add(child)
    for n in ['sub_001F6710','sub_001F6D80','sub_001F6970','sub_001E3520']:add(n)
    manual=(ROOT/'ports/mercenaries/src/recomp_manual.c').read_text()
    context=manual[manual.index('typedef struct recomp_saved_guest_cpu_context'):manual.index('#include "voice_callback_queue.h"')]
    fallback=manual[manual.index('typedef struct merc_lua_pool_block'):manual.index('/*\n * Lua 5 luaS_newlstr')]
    fallback=fallback.replace('if (result || !size) return result;','if (result || !size) return result;\n#ifdef LEGACY\nreturn 0;\n#endif')
    fixture='#define RECOMP_GENERATED_CODE\n#include "'+(ROOT/'ports/mercenaries/src/recomp/recomp_types.h').as_posix()+'"\n'+PRELUDE+context+'\n'.join('void '+n+'(void);' for n in selected)+fallback+'\n'.join(funcs[n] for n in selected)+HARNESS
    with tempfile.TemporaryDirectory(prefix='merc-lua-pool-') as td:
        p=Path(td);(p/'probe.c').write_text(fixture)
        for legacy in [True,False]:
            exe=p/('old.exe' if legacy else 'fixed.exe')
            if args.msvc:
                cmd=['cl','/nologo','/O2','/std:c11','/I'+str(ROOT/'ports/mercenaries/src'),*(['/DLEGACY'] if legacy else []),str(p/'probe.c'),'/Fe:'+str(exe),'/Fo:'+str(p/'probe.obj')]
            else:
                cmd=['C:/MinGW/bin/gcc.exe','-O2','-fno-strict-aliasing','-I',str(ROOT/'ports/mercenaries/src'),*(['-DLEGACY'] if legacy else []),str(p/'probe.c'),'-o',str(exe)]
            subprocess.run(cmd,check=True)
            subprocess.run([str(exe),*([str(args.snapshot.resolve())] if args.snapshot else [])],check=True,timeout=30)
if __name__=='__main__':main()
