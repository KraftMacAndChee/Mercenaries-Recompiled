"""Execute the money HUD at varying rates against its unpatched 30 FPS behavior."""
from pathlib import Path
import re
import runpy
import shutil
import subprocess
import tempfile
import unittest
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from tools.recomp.generated_test_utils import generated_text_containing


class MoneyTimingTests(unittest.TestCase):
    def test_money_counter_and_cues_keep_retail_cadence(self):
        source = generated_text_containing('void sub_000F5300(void)\n{')
        fixed = re.search(r'void sub_000F5300\(void\)\n\{.*?\n\}', source, re.S)[0]
        patches = runpy.run_path(str(ROOT / 'ports/mercenaries/scripts/Patch-Generated.py'))['PATCHES']
        original = fixed
        for patch in reversed(patches):
            if patch.name.startswith('Money HUD '):
                self.assertEqual(original.count(patch.after), 1)
                original = original.replace(patch.after, patch.before)
        self.assertNotEqual(original, fixed)
        # Reapplying the new patches is exact and a second application is a no-op.
        replay = original
        for patch in patches:
            if patch.name.startswith('Money HUD '):
                self.assertEqual(replay.count(patch.before), 1)
                replay = replay.replace(patch.before, patch.after)
        self.assertEqual(replay, fixed)
        for patch in patches:
            if patch.name.startswith('Money HUD '):
                self.assertEqual(replay.count(patch.after), 1)
                self.assertNotIn(patch.before, replay)
        manual = (ROOT / 'ports/mercenaries/src/recomp_manual.c').read_text(encoding='utf-8')
        helper = re.search(r'float recomp_money_counter_dt\(.*?\n\}', manual, re.S)[0]
        prefix = r'''
#define RECOMP_GENERATED_CODE
#include "recomp/recomp_types.h"
#include <assert.h>
#include <stdio.h>
uint32_t g_eax,g_ecx,g_edx,g_esp,g_ebx,g_esi,g_edi,g_seh_ebp,g_fp_top;
ptrdiff_t g_xbox_mem_offset;
double g_fp_stack[8];
float g_xmm0[4],g_xmm1[4],g_xmm2[4],g_xmm3[4],g_xmm4[4],g_xmm5[4],g_xmm6[4],g_xmm7[4];
static unsigned char memory[0x800000];
#undef RECOMP_TRACE_FUNC
#define RECOMP_TRACE_FUNC(a) ((void)0)
enum { STATE=0x10000, PLAYER=0x20000, STACK=0x3e0000 };
static unsigned sounds;
static void sub_0008ECE0(void) { eax=PLAYER; esp+=4; }
static void sub_0008C7E0(void) { eax=PLAYER; esp+=4; }
static void sub_00152580(void) {
    assert(MEM32(esp+4)==0x50ff6ac4); ++sounds; esp+=8;
}
/* Preload the retail target/rate: this test covers counting, sound dispatch,
 * fading and ABI, not the unchanged CRT rate-initialization helpers. */
static void sub_00237629(void) { assert(0); }
static void sub_002375B4(void) { assert(0); }
'''
        tail = r'''
static void step(void (*update)(void), float dt, float game_scale) {
    ecx=STATE; esp=STACK; esi=0x12345678; edi=0x3456789a;
    MEM32(esp)=0xBADCAFE; MEMF(esp+4)=dt*game_scale; MEMF(0x413F98)=dt;
    unsigned previous=sounds;
    update();
    assert(esp==STACK+8 && esi==0x12345678 && edi==0x3456789a && g_fp_top==0);
    assert(MEM32(STACK)==0xBADCAFE && sounds-previous<=1);
}
static void setup(int direction) {
    memset(memory+STATE,0,0x100);
    MEM32(STATE+0x40)=direction<0 ? 6000 : 0;
    MEM32(STATE+0x44)=3000;
    MEM32(STATE+0x48)=(uint32_t)(direction*1000);
    MEMF(STATE+0x38)=2; MEMF(STATE+0x3c)=1;
    MEMF(PLAYER+0xac4)=3000; sounds=0;
}
static double run(unsigned fps, int direction, float game_scale, int jitter) {
    setup(direction);
    double time=0;
    unsigned frames=0,changes=0;
    while(time<1.0-1e-7) {
        float dt=jitter ? (frames%2 ? 1.f/80 : 1.f/240) : 1.f/fps;
        int previous=SMEM32(STATE+0x40);
        step(sub_000F5300,dt,game_scale);
        changes += previous!=SMEM32(STATE+0x40);
        time+=dt; ++frames;
    }
    assert(sounds==15 && changes==30);
    assert(fabs((double)SMEM32(STATE+0x40)-(direction>0 ? 0 : 6000)
                -direction*990*(1+game_scale))<=32);
    while(MEM32(STATE+0x48) && time<7) {
        float dt=1.f/fps;
        step(sub_000F5300,dt,game_scale); time+=dt;
    }
    assert(MEM32(STATE+0x40)==3000 && MEM32(STATE+0x48)==0);
    unsigned final_sounds=sounds;
    while(MEMF(STATE+(direction>0 ? 0x30 : 0x34))>0 && time<14) {
        step(sub_000F5300,1.f/fps,game_scale); time+=1.0/fps;
    }
    assert(sounds==final_sounds);
    assert(time<14 && MEMF(PLAYER+0xac4)==3000);
    printf("%u FPS %s scale=%.1f%s: 30 digit changes / 15 cues in first second; effect ends %.3fs\n",
           fps,direction>0?"gain":"spend",game_scale,jitter?" variable":"",time);
    return time;
}
int main(void) {
    g_xbox_mem_offset=(ptrdiff_t)memory;
    /* Constants read from the retail XBE at the addresses used by the HUD. */
    MEM32(0x2DC08C)=0x3F800000u; MEM32(0x2DBED0)=0x40400000u;
    MEM32(0x2DC3F0)=0x3FC00000u; MEM32(0x2DDA48)=0x3EAAAAABu;
    MEM32(0x2EB6AC)=0x40000000u;
    unsigned char reference[180][0x60]; unsigned cue_reference[180];
    for(int direction=-1;direction<=1;direction+=2) {
        setup(direction);
        for(unsigned i=0;i<180;i++) {
            step(original_money_update,1.f/30,1);
            memcpy(reference[i],memory+STATE,0x60); cue_reference[i]=sounds;
        }
        setup(direction);
        for(unsigned i=0;i<180;i++) {
            step(sub_000F5300,1.f/30,1);
            assert(!memcmp(reference[i],memory+STATE,0x60));
            assert(sounds==cue_reference[i]);
        }
    }
    /* Establish the original defect, not just an expected result for the fix. */
    setup(1);
    for(unsigned i=0;i<120;i++) step(original_money_update,1.f/120,1);
    assert(sounds==60);
    const unsigned rates[]={30,60,90,120,144,240,1000};
    for(int direction=-1;direction<=1;direction+=2) {
        double baseline=run(30,direction,1,0);
        for(unsigned i=1;i<sizeof(rates)/sizeof(rates[0]);i++)
            assert(fabs(run(rates[i],direction,1,0)-baseline)<.10);
        assert(fabs(run(120,direction,1,1)-baseline)<.10);
        run(120,direction,0,0);  /* paused game: UI time still advances */
        run(120,direction,.5f,0);
    }
    setup(1);
    for(unsigned i=0;i<100;i++) step(sub_000F5300,0,0);
    assert(!sounds && !MEM32(STATE+0x40));
    step(sub_000F5300,.25f,1);  /* no multi-cue burst after a hitch */
    assert(sounds==1 && MEM32(STATE+0x40)==500);
    MEM32(STATE+0x58)=0xFFFFFFFEu;
    for(unsigned i=0;i<8;i++) step(sub_000F5300,1.f/120,1);
    assert(MEM32(STATE+0x58)==6);
    setup(1); /* reset in place must discard the previous fractional phase */
    for(unsigned i=0;i<3;i++) step(sub_000F5300,1.f/120,1);
    assert(!sounds && !MEM32(STATE+0x40));
    step(sub_000F5300,1.f/120,1);
    assert(sounds==1 && MEM32(STATE+0x40)==66);
    puts("30 FPS widget state/cues match retail exactly; high-FPS cadence, fades, pause, hitch, reset, wrap and guest ABI pass.");
}
'''
        with tempfile.TemporaryDirectory(prefix='merc-money-timing-') as directory:
            path = Path(directory)
            c, exe = path / 'test.c', path / 'test.exe'
            c.write_text(prefix + helper + original.replace('sub_000F5300', 'original_money_update') + fixed + tail, encoding='utf-8')
            compiler = shutil.which('gcc') or 'C:/MinGW/bin/gcc.exe'
            subprocess.run([compiler, '-O2', '-msse2', '-mfpmath=sse', '-fno-strict-aliasing',
                            '-I', str(ROOT / 'ports/mercenaries/src'), str(c), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    unittest.main()
