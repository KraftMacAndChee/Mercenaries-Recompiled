"""Execute the production PblThread scalar destructor with adversarial stack data."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from generated_test_utils import generated_text_containing

ROOT = Path(__file__).resolve().parents[2]

PRELUDE = r'''
#include <assert.h>
#include <stdlib.h>
#undef assert
#define assert(condition) do { if (!(condition)) exit(1); } while (0)
#include <stdint.h>
static uint8_t memory[0x20000];
static uint32_t eax, ecx, esi, esp;
static unsigned frees;
#define MEM32(a) (*(uint32_t *)(void *)(memory + (uint32_t)(a)))
#define MEM8(a) memory[(uint32_t)(a)]
#define PUSH32(s,v) do { uint32_t v_=(v); (s)-=4; MEM32(s)=v_; } while(0)
#define POP32(s,v) do { (v)=MEM32(s); (s)+=4; } while(0)
#define TEST_Z(a,b) (((uint32_t)(a)&(uint32_t)(b))==0)
#define RECOMP_TRACE_FUNC(a) ((void)0)
static void sub_001F6B60(void) {
    assert(MEM32(esp+4)==0x1000); ++frees; esp+=4;
}
'''
HARNESS = r'''
static void check(unsigned flag, unsigned return_word, unsigned saved) {
    frees=0; esp=0x10000; ecx=0x1000; esi=saved;
    PUSH32(esp,flag); PUSH32(esp,return_word);
    sub_001FA0D0();
    assert(frees==(flag&1)); assert(esp==0x10000);
    assert(esi==saved && eax==0x1000 && MEM32(0x1000)==0x300DE0);
}
int main(void) {
    for(unsigned flag=0;flag<4;++flag)
        for(unsigned ret=0;ret<2;++ret)
            for(unsigned saved=0; saved<2; ++saved) check(flag,ret,saved);
    return 0;
}
'''

class ThreadDestructorTests(unittest.TestCase):
    def test_real_function_and_old_code_negative_control(self):
        compiler = shutil.which("gcc") or "C:/MinGW/bin/gcc.exe"
        if not Path(compiler).exists():
            self.skipTest("GCC is required")
        text=generated_text_containing("void sub_001FA0D0(void)")
        start=text.index("void sub_001FA0D0(void)")
        fixed=text[start:text.index("\n}\n",start)+3]
        custom=("    _flags = TEST_Z(MEM8(esp + 4), 1); "
                "/* preserve delete flag before PUSH changes ESP */\n")
        native=("    _flags = (TEST_Z(MEM8(esp + 4), 1)); "
                "/* preserve test flags across 3 instruction(s) */\n")
        snapshot = native if native in fixed else custom
        self.assertIn(snapshot,fixed)
        old=fixed.replace(snapshot,"").replace(
            "if (_flags != 0)", "if (TEST_Z(MEM8(esp + 4), 1))"
        )
        for label,code in (("fixed",fixed),("old",old)):
            with self.subTest(label=label), tempfile.TemporaryDirectory(prefix="mercs-thread-dtor-") as tmp:
                source=Path(tmp)/"dtor.c"; executable=Path(tmp)/"dtor.exe"
                source.write_text(PRELUDE+code+HARNESS,encoding="utf-8")
                subprocess.run([compiler,"-std=c11",str(source),"-o",str(executable)],check=True,capture_output=True)
                result=subprocess.run([str(executable)],capture_output=True)
                if label=="fixed": self.assertEqual(result.returncode,0,result.stderr.decode(errors="replace"))
                else: self.assertNotEqual(result.returncode,0,"Old destructor flag bug escaped detection")

if __name__=="__main__": unittest.main()
