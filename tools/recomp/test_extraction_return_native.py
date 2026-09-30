"""Run the retail extraction incoming state, preserving its actual shared frame."""
from pathlib import Path
import re
import runpy
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from tools.recomp import config
from generated_test_utils import generated_text_containing


class ExtractionReturnTests(unittest.TestCase):
    def test_retail_epilogue_and_incoming_wait_path(self):
        xbe = ROOT / 'game_files/mercenaries-retail/default.xbe'
        config.configure_from_xbe(str(xbe))
        raw = xbe.read_bytes()
        offset = config.va_to_file_offset(0x7CDF7)
        self.assertEqual(raw[offset:offset + 8], bytes.fromhex('5f5e83c41cc20400'))
        patch_state = runpy.run_path(
            str(ROOT / 'ports/mercenaries/scripts/Patch-Generated.py')
        )
        patch = next(
            p for p in patch_state['PATCHES']
            if p.name == 'Retail extraction helicopter shared return restores its full frame'
        )
        code = generated_text_containing('loc_0007CBC1: ;')
        inline = patch_state['TRANSLATOR_INLINED_PATCHES'][patch.name]
        self.assertIn(inline, code)
        branch = code[
            code.index('loc_0007CBC1: ;'):code.index('loc_0007CC41: ;')
        ]
        old_inline = 'loc_0007CDF7: ;\n    esp += 4; return; /* stale minimal ret */'

        def incoming_with(epilogue):
            return (
                'static void sub_0007CBC1(void) { int _flags = 0;\n'
                + branch + '\n' + epilogue + '\n}\n'
            )

        fixed = incoming_with(inline)
        old = incoming_with(old_inline)
        prelude = r'''
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#define CHECK(c) do {if(!(c))exit(2);}while(0)
static unsigned char memory[0x300000];
static uint32_t eax,ecx,edx,esi,edi,esp;
static unsigned move_status,voice_calls;
static float xmm0v[4];
#define xmm0 xmm0v[0]
#define g_esp esp
#define MEM32(a) (*(uint32_t *)(void *)(memory+(uint32_t)(a)))
#define MEMF(a) (*(float *)(void *)(memory+(uint32_t)(a)))
#define PUSH32(s,v) do{uint32_t v_=(v);(s)-=4;MEM32(s)=v_;}while(0)
#define POP32(s,v) do{(v)=MEM32(s);(s)+=4;}while(0)
#define CMP_NE(a,b) ((uint32_t)(a)!=(uint32_t)(b))
#define RECOMP_TRACE_FUNC(a) ((void)0)
#define recomp_xmm_loadss(v,a) ((v)[0]=MEMF(a))
static void sub_000785E0(void) {
 CHECK(ecx==0x2000);CHECK(MEM32(esp+12)==0x2100);
 CHECK(MEM32(esp+16)==0x41a00000&&MEM32(esp+20)==0x41a00000);
 CHECK(MEM32(esp+24)==0x41400000);
 eax=move_status;esp+=28;
}
static void radio(uint32_t target) {
 CHECK(target==0x123456&&ecx==0x2000&&MEM32(esp+4)==1);
 ++voice_calls;esp+=8;
}
#define RECOMP_ICALL_SAFE(target,saved) radio(target)
'''
        harness = r'''
int main(void) {
 unsigned cases=0;
 for(unsigned saved=0;saved<16;++saved)for(move_status=0;move_status<6;++move_status) {
  memset(memory,0,sizeof(memory));voice_calls=0;
  esp=0x10000;esi=0x40000000+saved;edi=0x41000000+saved;
  PUSH32(esp,0x3dcccccd);PUSH32(esp,0x12345678);esp-=0x1c;
  PUSH32(esp,esi);PUSH32(esp,edi);esi=0x1000;edi=0x3000;
  MEM32(esi+8)=0x2000;MEM32(0x2000)=0x4000;MEM32(0x4000+0x18c)=0x123456;
  MEMF(esi+0xc)=1572;MEMF(esi+0x10)=74;MEMF(esi+0x14)=666;
  MEMF(0x2e429c)=12;MEMF(0x2dc8b0)=16;
  sub_0007CBC1();
  if(esp!=0x10000||esi!=0x40000000+saved||edi!=0x41000000+saved)return 3;
  CHECK(MEM32(0x1018)==(move_status==4));
  CHECK(voice_calls==(move_status==4));++cases;
 }
 printf("%u extraction incoming/wait/arrival cases passed\n",cases);return 0;
}
'''
        compiler = shutil.which('gcc') or 'C:/MinGW/bin/gcc.exe'
        for name, incoming in (('fixed', fixed), ('old', old)):
            with self.subTest(name=name), tempfile.TemporaryDirectory(prefix='merc-extract-') as directory:
                path = Path(directory)
                (path / 'test.c').write_text(prelude + incoming + harness)
                exe = path / 'test.exe'
                subprocess.run([compiler, '-O2', '-std=c11', str(path / 'test.c'), '-o', str(exe)], check=True)
                result = subprocess.run([str(exe)], capture_output=True, text=True)
                if name == 'fixed':
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    print(result.stdout.strip())
                else:
                    self.assertEqual(result.returncode, 3, 'Old stub must fail saved frame/ESP checks')


if __name__ == '__main__':
    unittest.main()
