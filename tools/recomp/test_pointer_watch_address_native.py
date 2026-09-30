"""Exercise diagnostic watch-address validation without setting real breakpoints."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class PointerWatchAddressTests(unittest.TestCase):
    def test_only_aligned_guest_words_can_be_watched(self):
        text = (ROOT / "ports/mercenaries/src/main.c").read_text(encoding="utf-8")
        start = text.index('    {\n        const char *watch_text = getenv("MERCENARIES_TRACE_POINTER_WATCH_VA");')
        end = text.index('\n\n    printf("Initializing NV2A MMIO bridge', start)
        body = text[start:end]
        prelude = r'''
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#define assert(c) do { if (!(c)) exit(1); } while (0)
static const char *custom, *legacy;
static unsigned calls, address;
static int g_global_pointer_watchpoint_custom;
static const char *fake_getenv(const char *name) {
 if (!strcmp(name,"MERCENARIES_TRACE_POINTER_WATCH_VA")) return custom;
 assert(!strcmp(name,"MERCENARIES_TRACE_GLOBAL_POINTER_WATCH")); return legacy;
}
#define getenv fake_getenv
static void arm_global_pointer_watchpoint(uint32_t value) { ++calls; address=value; }
'''
        harness = r'''
static void check(const char *value,const char *old,unsigned expected,int mode) {
 custom=value;legacy=old;calls=address=g_global_pointer_watchpoint_custom=0;
 configure_watch();
 assert(calls==(expected!=0)&&address==expected&&g_global_pointer_watchpoint_custom==mode);
}
int main(void) {
 check(NULL,NULL,0,0);check(NULL,"1",0x643844,0);
 check("","1",0x643844,0);
 check("0x10000",NULL,0x10000,1);
 check("0x03fffffc",NULL,0x3fffffc,1);
 check("10340252","1",10340252,1);
 const char *bad[]={"0","0xffff","0x10001","0x10002","0x10003",
 "0x4000000","0x90a7c7d0","-1","no","0x10000junk",
 "0x10000 4","0x100000000","99999999999999999999999999"};
 for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);++i) {
  check(bad[i],NULL,0,0);check(bad[i],"1",0,0);
 }
 return 0;
}
'''
        compiler = shutil.which("gcc") or "C:/MinGW/bin/gcc.exe"
        if not Path(compiler).exists():
            self.skipTest("GCC required")
        with tempfile.TemporaryDirectory(prefix="mercs-watch-address-") as directory:
            source = Path(directory) / "check.c"
            executable = Path(directory) / "check.exe"
            source.write_text(prelude + "\nstatic void configure_watch(void)\n" + body + harness, encoding="utf-8")
            compiled = subprocess.run([compiler, "-std=c11", str(source), "-o", str(executable)], capture_output=True)
            self.assertEqual(compiled.returncode, 0, compiled.stderr.decode(errors="replace"))
            tested = subprocess.run([str(executable)], capture_output=True)
            self.assertEqual(tested.returncode, 0, tested.stderr.decode(errors="replace"))


if __name__ == "__main__":
    unittest.main()
