"""Check extraction diagnostics are bounded, opt-in and read-only."""
from pathlib import Path
import re
import runpy
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class ExtractionProbeTests(unittest.TestCase):
    def test_generated_round_trip(self):
        module = runpy.run_path(str(ROOT / 'ports/mercenaries/scripts/Patch-Generated.py'))
        instrument = module['trace_extraction_flight']
        total = 0
        for unit in ('0000', '0002'):
            text = (ROOT / f'ports/mercenaries/src/recomp/gen/recomp_{unit}.c').read_text()
            stripped, count = re.subn(
                r'    \{ extern void recomp_extraction_probe\(uint32_t, uint32_t, uint32_t\);\n'
                r'      recomp_extraction_probe\([^\n]+\); \}\n', '', text)
            total += count
            self.assertEqual(instrument(stripped), text)
            self.assertEqual(instrument(text), text)
        self.assertEqual(total, 6)

    def test_native_guards_and_state_preservation(self):
        source = (ROOT / 'ports/mercenaries/src/recomp_manual.c').read_text()
        function = re.search(r'void recomp_extraction_probe\(.*?\n\}', source, re.S)[0]
        prelude = r'''
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#define CHECK(c) do {if(!(c))exit(2);}while(0)
static unsigned reads,reports,queries;
static const char *setting;
static uint32_t guest_u32(uint32_t a) {++reads;return a==0x10008?0x20000:0x30000;}
static float guest_f32(uint32_t a) {++reads;return 1.0f;}
static uint8_t guest_u8(uint32_t a) {++reads;return 0;}
static char *test_getenv(const char *name) {
 CHECK(!strcmp(name,"MERCENARIES_TRACE_EXTRACTION_FLIGHT"));++queries;return (char*)setting;
}
static int test_fprintf(FILE *file,const char *format,...) {++reports;return 0;}
#define getenv test_getenv
#define fprintf test_fprintf
'''
        harness = r'''
int main(int argc,char **argv) {
 setting=argc>1?"1":NULL;
 for(unsigned i=0;i<3000;++i) {
  recomp_extraction_probe(6,0x10000,0);
  recomp_extraction_probe(0,0,0);
  recomp_extraction_probe(0,0x3fffffc,0);
 }
 CHECK(queries==1&&reads==0&&reports==0);
 for(unsigned i=0;i<3000;++i) {
  for(unsigned stage=0;stage<6;++stage)recomp_extraction_probe(stage,0x10000,0);
 }
 CHECK(reports==(setting?64*5+256:0));
 CHECK(setting||reads==0);
 return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='merc-extraction-probe-') as directory:
            path = Path(directory)
            (path / 'test.c').write_text(prelude + function + harness)
            exe = path / 'test.exe'
            subprocess.run([shutil.which('gcc') or 'C:/MinGW/bin/gcc.exe', '-O2', '-std=c11',
                            str(path / 'test.c'), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)
            subprocess.run([str(exe), 'enabled'], check=True)

    def test_extraction_use_generated_round_trip(self):
        module = runpy.run_path(str(ROOT / 'ports/mercenaries/scripts/Patch-Generated.py'))
        instrument = module['trace_extraction_use']
        text = (ROOT / 'ports/mercenaries/src/recomp/gen/recomp_0001.c').read_text()
        stripped, count = re.subn(
            r'    \{ extern void recomp_extraction_use_probe\(uint32_t, uint32_t, '
            r'uint32_t, uint32_t, uint32_t\);\n'
            r'      recomp_extraction_use_probe\([^\n]+\); \}\n', '', text)
        self.assertEqual(count, 5)
        self.assertEqual(instrument(stripped), text)
        self.assertEqual(instrument(text), text)

    def test_extraction_use_native_guards_and_bound(self):
        source = (ROOT / 'ports/mercenaries/src/recomp_manual.c').read_text()
        function = re.search(
            r'void recomp_extraction_use_probe\(.*?\n\}', source, re.S)[0]
        prelude = r'''
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#define CHECK(c) do {if(!(c))exit(2);}while(0)
static unsigned reads,reports,queries,saw_data,saw_bad_data;
static const char *setting;
static uint32_t guest_u32(uint32_t a) {++reads;return a==0x10000u?0x002E21A8u:0;}
static float guest_f32(uint32_t a) {
 ++reads;
 if(a==0x30000u)saw_data=1;
 if(a==0x30030u)saw_bad_data=1;
 return 1.0f;
}
static uint8_t guest_u8(uint32_t a) {++reads;return a==0x11cedu;}
static char *test_getenv(const char *name) {
 CHECK(!strcmp(name,"MERCENARIES_TRACE_EXTRACTION_USE"));++queries;return (char*)setting;
}
static int test_fprintf(FILE *file,const char *format,...) {++reports;return 0;}
#define getenv test_getenv
#define fprintf test_fprintf
'''
        harness = r'''
int main(int argc,char **argv) {
 setting=argc>1?"1":NULL;
 for(unsigned i=0;i<3000;++i)
  recomp_extraction_use_probe(0,0x10000,0x20000,0x30000,0);
 CHECK(queries==1);
 CHECK(reports==(setting?382:0));
 CHECK(setting||reads==0);
 CHECK(!setting||(saw_data&&!saw_bad_data));
 return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='merc-extraction-use-') as directory:
            path = Path(directory)
            (path / 'test.c').write_text(prelude + function + harness)
            exe = path / 'test.exe'
            subprocess.run([shutil.which('gcc') or 'C:/MinGW/bin/gcc.exe', '-O2', '-std=c11',
                            str(path / 'test.c'), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)
            subprocess.run([str(exe), 'enabled'], check=True)


if __name__ == '__main__':
    unittest.main()
