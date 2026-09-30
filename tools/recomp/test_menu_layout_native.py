"""Check measured custom-menu bounds without leaking guest CPU state."""
from pathlib import Path
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[2]
manual=(ROOT/'ports/mercenaries/src/recomp_manual.c').read_text()
cpu=manual[manual.index('typedef struct recomp_saved_guest_cpu_context'):manual.index('static void recomp_guest_push_u32')]
source='#define RECOMP_GENERATED_CODE\n#include "'+(ROOT/'ports/mercenaries/src/recomp/recomp_types.h').as_posix()+'"\n'+r"""
#include <assert.h>
#include <stdio.h>
#include <math.h>
uint32_t g_eax,g_ecx,g_edx,g_esp,g_ebx,g_esi,g_edi,g_seh_ebp,g_fp_top;
volatile uint32_t g_recomp_current_func;
uint16_t g_x87_control_word,g_x87_status_word;
uint64_t g_mm0,g_mm1,g_mm2,g_mm3,g_mm4,g_mm5,g_mm6,g_mm7;
ptrdiff_t g_xbox_mem_offset;double g_fp_stack[8];
float g_xmm0[4],g_xmm1[4],g_xmm2[4],g_xmm3[4],g_xmm4[4],g_xmm5[4],g_xmm6[4],g_xmm7[4];
static unsigned char memory[0x400000];
#define RECOMP_CONTROLS_ROW 100u
#define RECOMP_OPTIONS_FPS_HASH 200u
static void *guest_ptr(uint32_t a){return memory+a;}
static uint32_t guest_u32(uint32_t a){return MEM32(a);}
static void recomp_guest_push_u32(uint32_t v){PUSH32(g_esp,v);}
static const char *recomp_controls_label(uint32_t h){return h==100?"short":h==101?"binding":NULL;}
static const char *recomp_options_label(uint32_t h){return h==200?"short":h==201?"long":h==202?"invalid":NULL;}
static int recomp_controls_row_binding(uint32_t h,void*a,void*b){return h==101;}
static int calls;
void sub_0020BA60(void){
 assert(ecx==0x10000 && MEM32(esp+8)==0xa9512042 && MEM32(esp+12)==0x3f800000);
 const char *label=guest_ptr(MEM32(esp+4));calls++;
 float width=!strcmp(label,"long")?350:!strcmp(label,"binding")?280:!strcmp(label,"invalid")?NAN:30;
 eax=99;ecx=77;esi=88;edi=66;ebx=55;g_xmm0[0]=3;g_fp_top=5;g_fp_stack[5]=width;esp+=16;
}
"""+cpu+'\n#include "'+(ROOT/'ports/mercenaries/src/menu_layout_guest.h').as_posix()+'"\n'+r"""
static void reset(uint32_t first){
 memset(memory,0,sizeof(memory));g_xbox_mem_offset=(ptrdiff_t)memory;
 esp=0x300000;eax=1;ecx=2;esi=3;edi=4;ebx=5;g_xmm0[0]=6;g_fp_top=0;g_fp_stack[0]=7;
 MEM32(0x1003c)=2;MEM32(0x10040)=first;MEM32(0x10044)=first+1;calls=0;
}
static void check(float expected){
 recomp_saved_guest_cpu_context before={0},after={0};recomp_save_guest_cpu_context(&before);
 assert(recomp_custom_menu_width(0x10000)==expected);recomp_save_guest_cpu_context(&after);
 assert(!memcmp(&before,&after,sizeof(before)));
}
int main(void){
 reset(200);check(427);assert(calls==2);
 reset(100);check(379);assert(calls==2);
 reset(200);MEM32(0x10044)=202;check(245);assert(calls==2);
 reset(42);check(245);assert(!calls);
 reset(200);MEM32(0x1003c)=17;check(245);assert(!calls);
 reset(200);MEM32(0x10044)=999;check(245);assert(calls==1);
 assert(recomp_custom_menu_width(0)==245);
 puts("PASS: custom label measurement, glyph padding, retail-menu isolation and CPU preservation");
}
"""
with tempfile.TemporaryDirectory(prefix='menu-layout-') as directory:
    path=Path(directory);(path/'test.c').write_text(source);exe=path/'test.exe'
    subprocess.run(['C:/MinGW/bin/gcc.exe','-std=c11','-O1','-fno-strict-aliasing','-I'+str(ROOT/'ports/mercenaries/src'),str(path/'test.c'),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
