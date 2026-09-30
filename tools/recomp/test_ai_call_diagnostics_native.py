"""Check that opt-in AI ABI diagnostics observe, never alter, guest registers."""
from pathlib import Path
import re
import runpy
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class AiCallDiagnosticsTests(unittest.TestCase):
    def test_scoped_instrumentation_is_idempotent_and_preserves_calls(self):
        instrument = runpy.run_path(str(ROOT / 'ports/mercenaries/scripts/Patch-Generated.py'))['trace_ai_update_calls']
        text = (ROOT / 'ports/mercenaries/src/recomp/gen/recomp_0002.c').read_text()
        self.assertEqual(instrument(text), text)
        wrapper = re.compile(
            r'    \{ const uint32_t _ai_esp = esp;\n'
            r'      const uint32_t _ai_esi = esi, _ai_edi = edi;\n'
            r'      extern void recomp_ai_call_checkpoint\(uint32_t, uint32_t, uint32_t, uint32_t\);\n'
            r'(    PUSH32\(esp, 0\);[^\n]+)\n'
            r'      recomp_ai_call_checkpoint\(0x[0-9A-F]+u, _ai_esp \+ \d+u, _ai_esi, _ai_edi\);\n'
            r'    \}')
        original, count = wrapper.subn(r'\1', text)
        self.assertEqual(count, 31)
        self.assertEqual(instrument(original), text)
        for name, body in re.findall(r'void sub_([0-9A-F]+)\(void\)\n(\{.*?\n\})', text, re.S):
            if 'recomp_ai_call_checkpoint(' in body:
                self.assertIn(name, ('0006C420', '00076540', '0006E930'))

    def test_native_default_off_and_bounded_read_only_reporting(self):
        source = (ROOT / 'ports/mercenaries/src/recomp_manual.c').read_text()
        function = re.search(r'void recomp_ai_call_checkpoint\(.*?\n\}', source, re.S)[0]
        prelude = r'''
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#define CHECK(c) do {if(!(c))exit(2);}while(0)
static uint32_t g_esp=0x1000,g_esi=0x2000,g_edi=0x3000;
static uint32_t g_eax=4,g_ecx=5,g_edx=6,g_ebx=7;
static unsigned reports,queries;
static const char *setting;
static char *test_getenv(const char *name) {
 CHECK(!strcmp(name,"MERCENARIES_TRACE_AI_UPDATE_STACK"));++queries;return (char *)setting;
}
static int test_fprintf(FILE *file,const char *format,...) {++reports;return 0;}
#define getenv test_getenv
#define fprintf test_fprintf
'''
        harness = r'''
int main(int argc,char **argv) {
 setting=argc>1?"1":NULL;
 recomp_ai_call_checkpoint(0x6C449,0x1000,0x2000,0x3000);
 CHECK(queries==0&&reports==0);
 for(unsigned i=0;i<100;++i) {
  recomp_ai_call_checkpoint(0x6C449,0x1004,0x2000,0x3000);
  recomp_ai_call_checkpoint(0x6C449,0x1000,0x2004,0x3000);
  recomp_ai_call_checkpoint(0x6C449,0x1000,0x2000,0x3004);
 }
 CHECK(queries==1&&reports==(setting?32:0));
 CHECK(g_esp==0x1000&&g_esi==0x2000&&g_edi==0x3000);
 CHECK(g_eax==4&&g_ecx==5&&g_edx==6&&g_ebx==7);
 return 0;
}
'''
        compiler = shutil.which('gcc') or 'C:/MinGW/bin/gcc.exe'
        with tempfile.TemporaryDirectory(prefix='merc-ai-abi-') as directory:
            path = Path(directory)
            (path / 'test.c').write_text(prelude + function + harness)
            exe = path / 'test.exe'
            subprocess.run([compiler, '-O2', '-std=c11', str(path / 'test.c'), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)
            subprocess.run([str(exe), 'enabled'], check=True)


if __name__ == '__main__':
    unittest.main()
