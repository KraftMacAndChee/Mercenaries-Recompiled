"""Replay title animations through the lifted retail setter and update routine."""
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

from generated_test_utils import generated_text_containing

ROOT = Path(__file__).resolve().parents[2]


class DatelineSound(unittest.TestCase):
    def test_repeated_and_shorter_titles(self):
        compiler = os.environ.get("CC") or shutil.which("gcc")
        if not compiler and Path("C:/MinGW/bin/gcc.exe").is_file():
            compiler = "C:/MinGW/bin/gcc.exe"
        if not compiler:
            self.skipTest("Set CC to a GCC-compatible C compiler")
        bodies = []
        for name in ("sub_00115500", "sub_000F5080"):
            source = generated_text_containing(f"void {name}(void)")
            bodies.append(re.search(rf"void {name}\(void\)\n\{{.*?\n\}}", source, re.S)[0])
        fixture = r'''
#include <assert.h>
#include <stdint.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
static int preserve = 1;
int recomp_options_og_bugs(void) { return preserve; }
#include "recomp_original_bugs.c"
static unsigned char memory[0x700000];
static uint32_t eax,ebx,ecx,edx,esi,edi,esp;
static float xmm0;
static unsigned ticks;
static int valid = 1;
static const char *title = "MISSION SK NW 2";
#define MEM32(a) (*(uint32_t *)(memory+(uint32_t)(a)))
#define MEM8(a) memory[(uint32_t)(a)]
#define MEMF(a) (*(float *)(memory+(uint32_t)(a)))
#define LO8(a) ((uint8_t)(a))
#define SET_LO8(a,b) ((a)=((a)&0xffffff00u)|(uint8_t)(b))
#define PUSH32(s,v) do { uint32_t value=(v); (s)-=4; MEM32(s)=value; } while(0)
#define POP32(s,v) do { (v)=MEM32(s); (s)+=4; } while(0)
#define TEST_Z(a,b) (((a)&(b))==0)
#define TEST_NZ(a,b) (((a)&(b))!=0)
#define CMP_LE(a,b) ((int32_t)(a)<=(int32_t)(b))
#define RECOMP_TRACE_FUNC(a) ((void)0)
#define xmm0v (&xmm0)
static void recomp_xmm_zero(float *v) { *v=0; }
static void recomp_xmm_loadss(float *v,uint32_t a) { *v=MEMF(a); }
static void sub_00113B60(void) { esp+=4; }
static void sub_00112AD0(void) { eax=valid?0x10000:0;esp+=4; }
static void sub_001F29F0(void) { eax=0x12345678;esp+=4; }
static void sub_001F7230(void) { eax=0x11000;esp+=4; }
static void sub_00121650(void) { eax=0x12000;esp+=4; }
static void sub_001211B0(void) {
    assert(MEM32(esp+12)==0x400);
    strcpy((char *)memory+MEM32(esp+8),title);esp+=16;
}
static void sub_00152580(void) {
    assert(MEM32(esp+4)==0x50FF6AC4 && ecx==0x37DD10);
    ticks++;esp+=8;
}
static void sub_001F66F0(void) { esp+=4; }
''' + "\n".join(bodies) + r'''
static void start(void) {
    esp=0x20000;esi=0x333;MEM32(esp+4)=0x10000;
    sub_00115500();assert(esp==0x20004 && esi==0x333);
}
static void step(float dt) {
    esp=0x20000;ecx=0x363EB8;esi=0x333;ebx=0x222;edi=0x444;
    MEMF(esp+4)=dt;sub_000F5080();
    assert(esp==0x20008 && esi==0x333 && ebx==0x222 && edi==0x444);
}
static unsigned animate(int fps) {
    unsigned before=ticks;
    for(int i=0;i<fps*8;i++)step(1.0f/fps);
    assert(MEM32(0x363EE4)==0 && MEMF(0x363EE8)==0);
    return ticks-before;
}
int main(void) {
    MEMF(0x2DDA34)=7;MEMF(0x2E219C)=1.0f/15;MEMF(0x2DC084)=15;
    /* Retail reproduces the report, even after the first title times out. */
    start();assert(animate(60)==strlen(title));
    start();assert(animate(60)==0);
    title="SHORT";start();assert(animate(60)==0);
    /* Optional correction restarts the sound alongside each title animation. */
    preserve=0;
    for(int fps=30;fps<=120;fps*=2) {
        title="MISSION SK NW 2";start();assert(animate(fps)==strlen(title));
        start();assert(animate(fps)==strlen(title));
        title="SHORT";start();assert(animate(fps)==strlen(title));
    }
    /* Reopening early also restarts; idle updates must not replay sounds. */
    start();step(0.2f);assert(MEM32(0x363EEC)>0);
    unsigned before=ticks;step(0);assert(ticks==before);
    start();assert(MEM32(0x363EEC)==0);assert(animate(60)==strlen(title));
    /* A failed Lua string read leaves the current title untouched. */
    start();step(0.2f);uint32_t chars=MEM32(0x363EEC);
    float timer=MEMF(0x363EE8);valid=0;start();
    assert(MEM32(0x363EEC)==chars && MEMF(0x363EE8)==timer);
    assert(MEM32(0x363EE4)==0x12345678);valid=1;
    /* Restoring OG Bugs retains the previous counter on the next title. */
    animate(60);preserve=1;start();assert(animate(60)==0);
    assert(!recomp_original_bug_fix_enabled((recomp_original_bug_fix)99));
    puts("PASS: retail missing ticks reproduced; optional repeated/shorter titles at 30/60/120 FPS");
}
'''
        with tempfile.TemporaryDirectory(prefix="merc-dateline-") as directory:
            folder = Path(directory)
            source = folder / "test.c"
            executable = folder / "test.exe"
            source.write_text(fixture, encoding="utf-8")
            subprocess.run([compiler, "-std=c11", "-O1",
                            "-I" + str(ROOT / "ports/mercenaries/src"),
                            str(source), "-o", str(executable), "-lm"], check=True)
            subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    unittest.main()
