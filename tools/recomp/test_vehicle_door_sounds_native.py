"""Execute the lifted retail door entry points and verify their spatial cue ABI."""

from pathlib import Path
import os
import re
import shutil
import struct
import subprocess
import tempfile
import unittest

from generated_test_utils import GEN, generated_text_containing
from tools.recomp import config

ROOT = Path(__file__).resolve().parents[2]


class VehicleDoorSounds(unittest.TestCase):
    def test_retail_vtables_and_generated_calls(self):
        xbe = ROOT / "game_files/mercenaries-retail/default.xbe"
        config.configure_from_xbe(str(xbe))
        data = xbe.read_bytes()
        dispatch = (GEN / "recomp_dispatch.c").read_text(encoding="utf-8")
        bodies = []
        for table, entry in ((0x2F30DC, 0x16D050), (0x2F30E8, 0x16D1B0)):
            offset = config.va_to_file_offset(table)
            self.assertEqual(struct.unpack_from("<I", data, offset)[0], entry)
            self.assertIn(f"0x{entry:08X}u, (recomp_func_t)sub_{entry:08X}", dispatch)
            source = generated_text_containing(f"void sub_{entry:08X}(void)")
            bodies.append(re.search(r"void sub_" + f"{entry:08X}" + r"\(void\)\n\{.*?\n\}", source, re.S)[0])
        compiler = os.environ.get("CC") or shutil.which("gcc")
        if not compiler and Path("C:/MinGW/bin/gcc.exe").is_file():
            compiler = "C:/MinGW/bin/gcc.exe"
        if not compiler:
            self.skipTest("Set CC to a GCC-compatible C compiler")
        fixture = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static unsigned char memory[0x4000000];
static uint32_t eax, ecx, edx, esp, calls, positions, expected_cue;
static float xmm0v[4], xmm1v[4];
#define g_esp esp
#define xmm0 xmm0v[0]
#define xmm1 xmm1v[0]
#define MEM32(a) (*(uint32_t *)(memory + (uint32_t)(a)))
#define MEMF(a) (*(float *)(memory + (uint32_t)(a)))
#define PUSH32(s,v) do { uint32_t value=(v); (s)-=4; MEM32(s)=value; } while(0)
#define TEST_Z(a,b) (((a)&(b))==0)
#define RECOMP_TRACE_FUNC(a) ((void)0)
static void recomp_xmm_zero(float *v) { memset(v,0,16); }
static void recomp_xmm_loadss(float *v,uint32_t a) { memset(v,0,16);v[0]=MEMF(a); }
static void get_position(uint32_t target) {
    assert(target==0x123456 && ecx==0x13000);
    eax=MEM32(esp+4); MEMF(eax)=10; MEMF(eax+4)=20; MEMF(eax+8)=30;
    esp+=8; positions++;
}
#define RECOMP_ICALL_SAFE(target, saved) get_position(target)
static void sub_001FFB30(void) {
    assert(MEM32(esp+4)==0);
    uint32_t p=MEM32(esp+8),v=MEM32(esp+12);
    assert(MEMF(p)==10 && MEMF(p+4)==20 && MEMF(p+8)==30);
    assert(MEMF(v)==0 && MEMF(v+4)==0 && MEMF(v+8)==0);
    assert(MEM32(esp+16)==expected_cue);
    calls++; esp+=4;
}
''' + "\n".join(bodies) + r'''
int main(void) {
    MEMF(0x2dc08c)=1;
    MEM32(0x10004)=0x11000;
    MEM32(0x11024)=0x12000; MEM32(0x12004)=0x13000;
    MEM32(0x13000)=0x14000; MEM32(0x14034)=0x123456;
    for (unsigned closing=0;closing<2;closing++) {
        for (unsigned audible=0;audible<2;audible++) {
            calls=positions=0;esp=0x20000;ecx=0x10000;
            expected_cue=audible ? (closing ? 0xc105e : 0x0a11ce) : 0;
            MEM32(0x11000+(closing?0x8c:0x88))=expected_cue;
            memset(memory+0x11030,0x55,64);MEMF(0x11074)=0.75f;
            if(closing)sub_0016D1B0();else sub_0016D050();
            assert(esp==0x20004 && calls==audible && positions==audible);
            if(!closing) {
                assert(MEMF(0x11074)==0);
                for(unsigned i=0;i<16;i++)assert(MEMF(0x11030+i*4)==(i%5==0 ? 1.0f:0.0f));
            } else assert(MEMF(0x11074)==0.75f);
        }
    }
    puts("PASS: retail door vtables, spatial cues, silent doors, pose and stack");
}
'''
        with tempfile.TemporaryDirectory(prefix="merc-door-sound-") as directory:
            source = Path(directory) / "door.c"
            executable = Path(directory) / "door.exe"
            source.write_text(fixture, encoding="utf-8")
            subprocess.run([compiler, "-std=c11", "-O2", str(source), "-o", str(executable)], check=True)
            subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    unittest.main()
