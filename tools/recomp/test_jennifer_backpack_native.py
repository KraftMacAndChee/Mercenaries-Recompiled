"""Exercise retail accessory creation and visibility restoration with OG Bugs on/off."""
from pathlib import Path
import os, re, runpy, shutil, subprocess, tempfile
from generated_test_utils import generated_text_containing
ROOT = Path(__file__).resolve().parents[2]
PORT = ROOT / "ports/mercenaries"
names = ["sub_00057600", "sub_00057AB0", "sub_00057AF0", "sub_00059D10"]
source = generated_text_containing("void sub_00057600(void)")
bodies = [re.search(r"void " + n + r"\(void\)\n\{.*?\n\}", source, re.S)[0] for n in names]
patches = runpy.run_path(str(PORT / "scripts/Patch-Generated.py"))["PATCHES"]
for patch in patches:
    if patch.name.startswith("Preserve alternate Jennifer backpack visibility"):
        assert patch.after in source
prelude = r"""
#define RECOMP_GENERATED_CODE
#include "recomp/recomp_types.h"
#include "recomp_original_bugs.c"
#include <assert.h>
#include <stdio.h>
#include <string.h>
uint32_t g_eax,g_ecx,g_edx,g_esp,g_ebx,g_esi,g_edi,g_seh_ebp,g_fp_top;
ptrdiff_t g_xbox_mem_offset;
double g_fp_stack[8];
float g_xmm0[4],g_xmm1[4],g_xmm2[4],g_xmm3[4],g_xmm4[4],g_xmm5[4],g_xmm6[4],g_xmm7[4];
static unsigned char memory[0x650000];
static int og;
int recomp_options_og_bugs(void){return og;}
#undef RECOMP_TRACE_FUNC
#define RECOMP_TRACE_FUNC(a) ((void)0)
#undef RECOMP_ICALL_SAFE
#define RECOMP_ICALL_SAFE(target,saved) virtual_call(target)
enum {PLAYER=0x10000,ACCESSORY=0x20000,VTABLE=0x30000,STACK=0x500000};
static void virtual_call(uint32_t target){
    switch(target){
    case 1: MEM32(ecx+0x100)=0;break;
    case 2: MEM32(ecx+0x100)=1;break;
    case 3: break;
    case 4: eax=1;break;
    case 5: eax=0x40000;break;
    default: assert(0);
    }
    esp+=4;
}
static void sub_001EB010(void){esp+=4;}
static void sub_001EAFB0(void){esp+=12;}
static void sub_001EB080(void){esp+=8;}
static void sub_00174480(void){eax=ACCESSORY;esp+=24;}
static void sub_00030B60(void){esp+=8;}
static void sub_001476A0(void){esp+=16;}
static void sub_00147670(void){esp+=8;}
"""
harness = r"""
static void prepare(void){
    esp=STACK;ecx=PLAYER;ebx=0x12345678;esi=0x123123;edi=0x444444;g_seh_ebp=0x889988;
}
static void check(void){assert(ebx==0x12345678&&esi==0x123123&&edi==0x444444&&g_seh_ebp==0x889988);}
int main(void){
    g_xbox_mem_offset=(ptrdiff_t)memory;
    MEM32(VTABLE+0x1CC)=1;MEM32(VTABLE+0x1D0)=2;MEM32(VTABLE+0x1DC)=3;
    MEM32(VTABLE+4)=4;MEM32(VTABLE+0x88)=5;MEM32(0x30C280)=1;MEM32(0x30C27C)=1;
    const uint32_t models[]={0x9FDBE410u,0x0FA8A321u,0u,0x12345678u};
    const uint32_t accessories[]={0x5120F434u,0x123u};
    unsigned cases=0;
    for(og=0;og<=1;og++)for(unsigned m=0;m<4;m++)for(unsigned a=0;a<2;a++){
        int expected=og||m!=0||a!=0;
        MEM32(PLAYER+0x58)=models[m];MEM32(ACCESSORY)=VTABLE;MEM32(ACCESSORY+0x58)=accessories[a];
        MEM32(PLAYER+0xAB4)=0;MEM32(PLAYER+0xE34)=0;
        prepare();MEM32(esp+4)=1;for(unsigned i=2;i<=6;i++)MEM32(esp+4*i)=0;
        sub_00059D10();assert(esp==STACK+28);check();
        assert(MEM32(ACCESSORY+0x100)==(unsigned)expected);
        assert(MEM32(PLAYER+0xAB4)==1&&MEM32(PLAYER+0xA9C)==ACCESSORY);
        /* Keep an unrelated accessory visible and exercise empty attachment slots. */
        MEM32(ACCESSORY+0x1000)=VTABLE;MEM32(ACCESSORY+0x1058)=0x5555;
        MEM32(PLAYER+0xAA0)=ACCESSORY+0x1000;MEM32(PLAYER+0xAA4)=0;MEM32(PLAYER+0xAB4)=3;
        for(unsigned repeat=0;repeat<20;repeat++){
            prepare();sub_00057AB0();assert(esp==STACK+4);check();
            assert(!MEM32(ACCESSORY+0x100)&&!MEM32(ACCESSORY+0x1100));
            prepare();sub_00057AF0();assert(esp==STACK+4);check();
            assert(MEM32(ACCESSORY+0x100)==(unsigned)expected&&MEM32(ACCESSORY+0x1100)==1);
            /* The boolean restore route has a dense array, unlike the sparse loop. */
            MEM32(PLAYER+0xAB4)=2;
            for(unsigned show=0;show<2;show++){
                prepare();MEM32(esp+4)=show;sub_00057600();assert(esp==STACK+8);check();
                assert(MEM32(ACCESSORY+0x100)==(unsigned)(show&&expected));
                assert(MEM32(ACCESSORY+0x1100)==show);
            }
            MEM32(PLAYER+0xAB4)=3;
        }
        cases++;
    }
    puts("PASS: 16 OG/costume/accessory combinations, creation and repeated hide/show routes; unrelated accessories and guest stack preserved");
    return 0;
}
"""
with tempfile.TemporaryDirectory(prefix="merc-backpack-") as temp:
    directory=Path(temp);c=directory/"test.c";exe=directory/"test.exe"
    c.write_text(prelude+"\n".join(bodies)+harness)
    subprocess.run([os.environ.get("CC") or shutil.which("gcc") or "C:/MinGW/bin/gcc.exe", "-std=c11", "-O2", "-fno-strict-aliasing", "-I", str(PORT/"src"), str(c), "-o", str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
