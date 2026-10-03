"""Run F9 relation commands through the retail matrix setter and production CPU save/restore."""
from pathlib import Path
import os,re,shutil,subprocess,tempfile
from generated_test_utils import generated_text_containing
ROOT=Path(__file__).resolve().parents[2]
PORT=ROOT/'ports/mercenaries/src'
def function(text,name):
    return re.search(r'(?:static )?(?:void|uint32_t) '+name+r'\([^;]*?\n\{.*?\n\}',text,re.S)[0]
manual=(PORT/'recomp_manual.c').read_text()
context=manual[manual.index('typedef struct recomp_saved_guest_cpu_context {'):manual.index('static void recomp_guard_cancel_owner',manual.index('typedef struct recomp_saved_guest_cpu_context {'))]
spawn=(PORT/'dev_spawn.h').read_text()
prelude=r"""
#define RECOMP_GENERATED_CODE
#include "recomp/recomp_types.h"
#include "dev_factions.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
uint32_t g_eax,g_ecx,g_edx,g_esp,g_ebx,g_esi,g_edi,g_seh_ebp,g_fp_top;
volatile uint32_t g_recomp_current_func;
uint16_t g_x87_control_word,g_x87_status_word;
uint64_t g_mm0,g_mm1,g_mm2,g_mm3,g_mm4,g_mm5,g_mm6,g_mm7;
ptrdiff_t g_xbox_mem_offset;
double g_fp_stack[8];
float g_xmm0[4],g_xmm1[4],g_xmm2[4],g_xmm3[4],g_xmm4[4],g_xmm5[4],g_xmm6[4],g_xmm7[4];
static unsigned char memory[0x600000];
static unsigned pending,calls;
static int allowed=1,available=1,success;
#undef RECOMP_TRACE_FUNC
#define RECOMP_TRACE_FUNC(a) ((void)0)
static uint32_t guest_u32(uint32_t a){return MEM32(a);}
static void *guest_ptr(uint32_t a){return memory+a;}
static uint32_t recomp_float_bits(float f){uint32_t b;memcpy(&b,&f,4);return b;}
unsigned recomp_dev_take_relation(void){unsigned c=pending;pending=0;return c;}
int recomp_dev_menu_allowed(void){return allowed;}
void recomp_dev_relation_result(int ok,const char *s){success=ok;assert(s&&*s);}
static void xbox_preview_log_event(const char *tag,const char *fmt,...){(void)tag;(void)fmt;}
"""
setter=function(generated_text_containing('void sub_00095B50(void)'),'sub_00095B50')
lookup=r"""
static void retail_set(void){calls++;sub_00095B50();}
recomp_func_t recomp_lookup(uint32_t a){assert(a==0x95B50);return available?retail_set:NULL;}
"""
tick=function((PORT/'dev_factions_guest.h').read_text(),'recomp_dev_relations_tick')
harness=r"""
enum {REL=0x323354,STACK=0x500000};
static void run(void){
    recomp_saved_guest_cpu_context before={0},after={0};
    recomp_save_guest_cpu_context(&before);recomp_dev_relations_tick();recomp_save_guest_cpu_context(&after);
    assert(!memcmp(&before,&after,sizeof(before)));assert(!pending);
}
int main(void){
    g_xbox_mem_offset=(ptrdiff_t)memory;g_esp=STACK;
    g_eax=111;g_ecx=222;g_edx=333;g_ebx=444;g_esi=555;g_edi=666;g_seh_ebp=777;
    g_fp_top=3;g_fp_stack[2]=123.456;g_xmm0[0]=9.25f;g_xmm3[2]=-8.f;
    g_mm0=0x123456789ABCDEF0ull;g_x87_control_word=0x37f;g_x87_status_word=0x2222;g_recomp_current_func=0x555;
    MEM32(0x413F6C)=0x4249D707;MEM32(0x413F68)=0xC2CBD863;
    for(unsigned a=0;a<7;a++)for(unsigned b=0;b<7;b++)for(unsigned status=0;status<4;status++){
        if(a==b){assert(!dev_relation_command(a,b,status));continue;}
        for(unsigned i=0;i<64;i++)MEMF(REL+4*i)=.123f;
        pending=dev_relation_command(a,b,status);calls=0;success=-1;run();assert(success==1&&calls==2);
        for(unsigned i=0;i<8;i++)for(unsigned j=0;j<8;j++){
            float expected=((i==a+1&&j==b+1)||(i==b+1&&j==a+1))?dev_relation_values[status]:.123f;
            assert(MEMF(REL+4*(i*8+j))==expected);
        }
    }
    assert(!dev_relation_command(7,1,0)&&!dev_relation_command(1,2,4));
    for(unsigned condition=0;condition<6;condition++){
        pending=dev_relation_command(0,5,3);calls=0;success=-1;
        if(condition==0)allowed=0;
        if(condition==1)available=0;
        if(condition==2)MEM32(0x413F68)=0;
        if(condition==3)MEM32(0x413F6C)=0;
        if(condition==4)g_esp=0x1000;
        if(condition==5)pending|=0x80000000u;
        run();assert(!success&&!calls);
        allowed=available=1;MEM32(0x413F6C)=0x4249D707;MEM32(0x413F68)=0xC2CBD863;g_esp=STACK;
    }
    success=-1;calls=0;run();assert(success==-1&&!calls);
    puts("PASS: all 168 faction/status commands update only the selected symmetric pair; invalid/game-state guards and full guest CPU preserved");
    return 0;
}
"""
with tempfile.TemporaryDirectory(prefix='merc-factions-') as temp:
    d=Path(temp);c=d/'test.c';exe=d/'test.exe'
    c.write_text(prelude+setter+lookup+context+function(spawn,'dev_call')+tick+harness)
    subprocess.run([os.environ.get('CC') or shutil.which('gcc') or 'C:/MinGW/bin/gcc.exe','-std=c11','-O2','-fno-strict-aliasing','-I',str(PORT),str(c),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
