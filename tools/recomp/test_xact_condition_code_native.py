"""Execute XACT accounting and variation flag lifetimes from retail-generated C."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "ports/mercenaries/src/recomp/gen/recomp_0012.c"
PRELUDE = r'''
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#define assert(c) do { if (!(c)) exit(1); } while (0)
static uint8_t memory[0x100000];
static uint32_t eax,ebx,ecx,edx,esi,edi,esp,ebp;
#define MEM32(a) (*(uint32_t *)(void *)(memory+(uint32_t)(a)))
#define MEM16(a) (*(uint16_t *)(void *)(memory+(uint32_t)(a)))
#define MEM8(a) memory[(uint32_t)(a)]
#define g_esp esp
#define LO8(a) ((uint8_t)(a))
#define ZX8(a) ((uint32_t)(uint8_t)(a))
#define ZX16(a) ((uint32_t)(uint16_t)(a))
#define PUSH32(s,v) do {uint32_t value_=(v); (s)-=4; MEM32(s)=value_;} while(0)
#define POP32(s,v) do {(v)=MEM32(s); (s)+=4;} while(0)
#define TEST_Z(a,b) (((uint32_t)(a)&(uint32_t)(b))==0)
#define TEST_NZ(a,b) (!TEST_Z(a,b))
#define CMP_EQ(a,b) ((uint32_t)(a)==(uint32_t)(b))
#define RECOMP_TRACE_FUNC(a) ((void)0)
#define RECOMP_ICALL_SAFE(a,b) assert(!"Unexpected critical-section release")
static void sub_0027D5C0(void) { eax=0; esp+=4; }
'''


class XactConditionCodeTests(unittest.TestCase):
    def execute(self, fixed, old, harness):
        compiler = shutil.which("gcc") or "C:/MinGW/bin/gcc.exe"
        if not Path(compiler).exists():
            self.skipTest("GCC required")
        self.assertNotEqual(fixed, old)
        for label, code in (("fixed", fixed), ("old", old)):
            with self.subTest(label=label), tempfile.TemporaryDirectory(prefix="mercs-xact-flags-") as directory:
                source = Path(directory) / "check.c"
                executable = Path(directory) / "check.exe"
                source.write_text(PRELUDE + code + harness, encoding="utf-8")
                result = subprocess.run([compiler, "-std=c11", str(source), "-o", str(executable)], capture_output=True)
                self.assertEqual(result.returncode, 0, result.stderr.decode(errors="replace"))
                result = subprocess.run([str(executable)], capture_output=True)
                if label == "fixed":
                    self.assertEqual(result.returncode, 0, result.stderr.decode(errors="replace"))
                else:
                    self.assertNotEqual(result.returncode, 0, "Original flag bug escaped detection")


    def test_sound_state_join_preserves_stop_comparison(self):
        text = SOURCE.read_text(encoding="utf-8")
        start = text.index("void sub_00285E40(void)")
        function = text[start:text.index("\n}\n", start)+3]
        helpers = r'''
#include <stdio.h>
#define LO16(a) ((uint16_t)(a))
#define CMP_L(a,b) ((int32_t)(a)<(int32_t)(b))
#define CMP_LE(a,b) ((int32_t)(a)<=(int32_t)(b))
static uint32_t g_seh_ebp;
static void sub_0027DB23(void) { esp+=4; }
static void sub_0027DB45(void) { esp+=4; }
'''
        fixed = helpers + function
        old = fixed.replace(
            "    if (CMP_LE(eax, 8)) goto loc_00285E96; /* already stopping/stopped */\n"
            "    goto loc_00285EA8;",
            "    goto loc_00285EA6;")
        # Truth table from retail 0x285E55..0x285EB9, including both callers
        # of the shared JLE. No asynchronous audio operations are stubbed here.
        self.execute(fixed, old, r'''
int main(void) {
 for(unsigned target=0;target<10;++target) for(unsigned prior=0;prior<10;++prior) {
  for(unsigned output=0;output<2;++output) {
   memset(memory,0,sizeof(memory));
   esp=0x90000; ecx=0x1000; esi=0xABCD; edi=0x1234;
   MEM16(0x1030)=prior; MEM32(esp+4)=target;
   MEM32(esp+8)=output?0x2000:0; MEM32(0x2000)=0xBAD;
   sub_00285E40();
   int blocked=(target==2&&(prior==4||prior==5)) ||
       ((target==4||target==5)&&prior==7) ||
       (target==6&&(prior==4||prior==5||prior==7)) ||
       (target==7&&(prior==7||prior==8));
   assert(eax==!blocked);
   assert(MEM16(0x1030)==(blocked?prior:target));
   assert(MEM32(0x2000)==((blocked||!output)?0xBAD:prior));
   assert(esp==0x9000C && esi==0xABCD && edi==0x1234);
  }
 }
 return 0;
}
''')

    def test_accounting_reads_resource_before_replacing_pointer_with_count(self):
        text = SOURCE.read_text(encoding="utf-8")
        start = text.index("void sub_0027DECC(void)")
        fixed = text[start:text.index("\n}\n", start)+3]
        old = fixed.replace("    _flags = (CMP_EQ(MEM32(eax + 0x1C), 0)); /* preserve cmp flags across 1 instruction(s) */\n", "")
        old = old.replace("if (_flags != 0)", "if (CMP_EQ(MEM32(eax + 0x1C), 0))")
        self.execute(fixed, old, r'''
int main(void) {
 const unsigned counts[]={0,1,20,64,65535};
 for(unsigned enabled=0;enabled<2;++enabled) for(unsigned i=0;i<5;++i) {
  memset(memory,0,sizeof(memory));
  ecx=0x1000; esi=0xABCD; esp=0x90000;
  MEM32(esp)=0xDEADBEEF; MEM32(esp+4)=0x2000;
  MEM32(0x2014)=0x20000000; MEM16(0x2010)=counts[i];
  MEM32(0x201C)=enabled;
  MEM32(counts[i]+0x1C)=!enabled;
  MEM32(0x1080)=100; MEM32(0x1088)=200;
  sub_0027DECC();
  assert(MEM32(0x1080)==100+(enabled?counts[i]:0));
  assert(MEM32(0x1088)==200+(enabled?0:counts[i]));
  assert(esp==0x90008 && esi==0xABCD);
 }
 return 0;
}
''')

    def test_variation_bitmap_uses_bit_index_before_loading_range_pointer(self):
        text = SOURCE.read_text(encoding="utf-8")
        start = text.index("loc_0027F872: ;")
        # Execute the real bitmap/range-adjustment blocks. Earlier random
        # selection and later cue construction are outside this branch test.
        block = text[start:text.index("loc_0027F89F: ;", start)]
        fixed = "static void variation(void) { int _flags=0,_cf=0;\n" + block + "loc_0027F89F: ;\n}\n"
        old = fixed.replace("    _flags = (TEST_Z(MEM32(edi + ecx * 4), edx)); /* preserve test flags across 1 instruction(s) */\n", "")
        old = old.replace("if (_flags != 0)", "if (TEST_Z(MEM32(edi + ecx * 4), edx))")
        self.execute(fixed, old, r'''
int main(void) {
 for(unsigned index=0;index<96;++index) for(unsigned used=0;used<2;++used) {
  memset(memory,0,sizeof(memory));
  eax=index; ebp=0x80000;
  MEM32(ebp-8)=0x200; MEM32(ebp+12)=0x3000; MEM32(ebp+8)=7;
  MEM16(0x2FFE)=5; MEM16(0x3000)=10;
  uint32_t bit=1u<<(index&31);
  MEM32(0x200+(index>>5)*4)=used?bit:0;
  MEM32(0x200+0x3000*4)=used?0:bit;
  variation();
  assert(MEM32(ebp+8)==7+(used?6:0));
  assert(ecx==0x3000 && eax==index);
 }
 return 0;
}
''')


if __name__ == "__main__":
    unittest.main()
