"""Execute retail human-region stack flags, including old-code negative controls."""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
PRELUDE = r'''
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#define assert(c) do {if (!(c)) exit(1);} while(0)
static uint8_t memory[0x20000];
static uint32_t eax,ecx,edx,esi,esp;
static unsigned frees,dead,subdued,queries,plays;
#define g_esp esp
#define MEM32(a) (*(uint32_t *)(void *)(memory+(uint32_t)(a)))
#define MEM8(a) memory[(uint32_t)(a)]
#define LO8(a) ((uint8_t)(a))
#define PUSH32(s,v) do {uint32_t value_=(v);(s)-=4;MEM32(s)=value_;} while(0)
#define POP32(s,v) do {(v)=MEM32(s);(s)+=4;} while(0)
#define TEST_Z(a,b) (((uint32_t)(a)&(uint32_t)(b))==0)
#define TEST_NZ(a,b) (!TEST_Z(a,b))
#define CMP_NE(a,b) ((uint32_t)(a)!=(uint32_t)(b))
#define RECOMP_TRACE_FUNC(a) ((void)0)
static void sub_001F6B60(void) {assert(MEM32(esp+4)==0x1000);++frees;esp+=4;}
static void query(unsigned target) {
 assert(ecx==0x1000);++queries;
 if(target==1)eax=dead;else {assert(target==2);eax=subdued;}
 ecx=0xBAD;esp+=4;
}
#define RECOMP_ICALL_SAFE(target,stack) query(target)
static void sub_00061E50(void) {
 assert(ecx==0x3000&&MEM32(esp+4)==0xA7189AE5&&MEM32(esp+8)==0&&MEM32(esp+12)==0);
 ++plays;esp+=16;
}
'''


class HumanStackFlagsTests(unittest.TestCase):
    def check_native(self, fixed, old, harness):
        compiler = shutil.which("gcc") or "C:/MinGW/bin/gcc.exe"
        if not Path(compiler).exists():
            self.skipTest("GCC required")
        self.assertNotEqual(fixed, old)
        for label, code in (("fixed", fixed), ("old", old)):
            with self.subTest(version=label), tempfile.TemporaryDirectory(prefix="mercs-human-flags-") as directory:
                source = Path(directory) / "check.c"
                executable = Path(directory) / "check.exe"
                source.write_text(PRELUDE + code + harness, encoding="utf-8")
                result = subprocess.run([compiler, "-std=c11", str(source), "-o", str(executable)], capture_output=True)
                self.assertEqual(result.returncode, 0, result.stderr.decode(errors="replace"))
                result = subprocess.run([str(executable)], capture_output=True)
                if label == "fixed":
                    self.assertEqual(result.returncode, 0, result.stderr.decode(errors="replace"))
                else:
                    self.assertNotEqual(result.returncode, 0, "Original stale-stack condition escaped detection")

    def function(self, address):
        text = (ROOT / "ports/mercenaries/src/recomp/gen/recomp_0001.c").read_text(encoding="utf-8")
        start = text.index(f"void sub_{address}(void)")
        return text[start:text.index("\n}\n", start) + 3]

    def test_scalar_destructors_use_argument_not_return_address(self):
        for address, vtable in (("00049AD0", "2E1EE0"), ("0004D160", "2E2480"), ("0005DDB0", "2E3734")):
            fixed = self.function(address)
            old = re.sub(r"    _flags = .*?preserve test flags.*?\n", "", fixed)
            old = old.replace("if (_flags != 0)", "if (TEST_Z(MEM8(esp + 4), 1))")
            harness = r'''
int main(void) {
 for(unsigned flag=0;flag<4;++flag)for(unsigned ret=0;ret<2;++ret)for(unsigned saved=0;saved<2;++saved) {
  frees=0;esp=0x10000;ecx=0x1000;esi=saved;
  PUSH32(esp,flag);PUSH32(esp,ret);FUNCTION();
  assert(frees==(flag&1)&&esp==0x10000&&esi==saved&&eax==0x1000);
  assert(MEM32(0x1000)==VTABLE);
 }
 return 0;
}
'''.replace("FUNCTION", "sub_" + address).replace("VTABLE", "0x" + vtable)
            with self.subTest(function=address):
                self.check_native(fixed, old, harness)

    def test_airstrike_reaction_uses_message_and_preserves_dead_subdued_checks(self):
        fixed = self.function("0004DB50")
        old = re.sub(r"    _flags = .*?preserve cmp flags.*?\n", "", fixed)
        old = old.replace("if (_flags != 0)", "if (CMP_NE(MEM32(esp + 4), 0xEC013C64u))")
        self.check_native(fixed, old, r'''
int main(void) {
 for(unsigned message=0;message<2;++message)for(unsigned ret=0;ret<2;++ret)
 for(dead=0;dead<2;++dead)for(subdued=0;subdued<2;++subdued)for(unsigned animator=0;animator<2;++animator) {
  memset(memory,0,sizeof(memory));plays=queries=0;
  esp=0x10000;ecx=0x1000;esi=0xABCD;
  MEM32(0x1000)=0x2000;MEM32(0x2000+0x1CC)=1;MEM32(0x2000+0x318)=2;
  MEM32(0x1000+0x6B0)=animator?0x3000:0;
  PUSH32(esp,message?0xEC013C64:0x12345678);PUSH32(esp,ret?0xEC013C64:0);
  sub_0004DB50();
  assert(esp==0x10000&&esi==0xABCD);
  assert(queries==(message?(dead?1:2):0));
  assert(plays==(message&&!dead&&!subdued&&animator));
 }
 return 0;
}
''')


if __name__ == "__main__":
    unittest.main()
