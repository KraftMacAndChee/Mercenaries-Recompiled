"""Execute every repaired 31-byte scalar deleting destructor against retail bytes."""
from pathlib import Path
import re
import runpy
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from tools.recomp import config


class ScalarDeleteTests(unittest.TestCase):
    def test_all_retail_templates_and_native_flag_combinations(self):
        xbe = ROOT / 'game_files/mercenaries-retail/default.xbe'
        config.configure_from_xbe(str(xbe))
        raw = xbe.read_bytes()
        functions = []
        retail_addresses = set()
        prefix = bytes.fromhex('F644240401568BF1C706')
        suffix = bytes.fromhex('83C4048BC65EC20400')
        for section in config._SECTIONS:
            if not section.is_code:
                continue
            first = section.raw_addr
            last = first + section.raw_size - 31
            for offset in range(first, last + 1):
                if raw[offset:offset + 10] != prefix:
                    continue
                if raw[offset + 14:offset + 18] != bytes.fromhex('740956E8'):
                    continue
                if raw[offset + 22:offset + 31] != suffix:
                    continue
                address = section.va + offset - section.raw_addr
                relative = struct.unpack_from('<i', raw, offset + 18)[0]
                if address + 22 + relative == 0x001F6B60:
                    retail_addresses.add(address)
        pattern = re.compile(r'void sub_([0-9A-F]{8})\(void\)\n\{.*?\n\}', re.S)
        repair = runpy.run_path(str(ROOT / 'ports/mercenaries/scripts/Patch-Generated.py'))['patch_simple_scalar_delete_flags']
        for path in (ROOT / 'ports/mercenaries/src/recomp/gen').glob('recomp_*.c'):
            text = path.read_text(encoding='utf-8')
            for match in pattern.finditer(text):
                body = match[0]
                custom = (
                    '    _flags = TEST_Z(MEM8(esp + 4), 1); '
                    '/* scalar-delete flag before PUSH */\n'
                )
                native = (
                    '    _flags = (TEST_Z(MEM8(esp + 4), 1)); '
                    '/* preserve test flags across 3 instruction(s) */\n'
                )
                if custom not in body and native not in body:
                    continue
                vtable_match = re.search(
                    r'MEM32\(esi\) = 0x([0-9A-F]+)', body
                )
                if vtable_match is None:
                    continue
                address = int(match[1], 16)
                vtable = int(vtable_match[1], 16)
                expected = (bytes.fromhex('F644240401568BF1C706') + struct.pack('<I', vtable)
                            + bytes.fromhex('740956E8') + struct.pack('<i', 0x1F6B60 - address - 22)
                            + bytes.fromhex('83C4048BC65EC20400'))
                offset = config.va_to_file_offset(address)
                if raw[offset:offset + 31] != expected:
                    continue
                if custom in body:
                    old = body.replace(custom, '')
                else:
                    old = body.replace(native, '')
                old = old.replace(
                    'if (_flags != 0)',
                    'if (TEST_Z(MEM8(esp + 4), 1))',
                )
                repaired = repair(old)
                self.assertNotEqual(repaired, old)
                self.assertIn('scalar-delete flag before PUSH', repaired)
                self.assertEqual(repair(body), body)
                # A different layout must be left for separate analysis.
                altered = old.replace('esi = ecx;', 'esi = ecx;\n    eax = 1;')
                self.assertEqual(repair(altered), altered)
                functions.append((match[1], vtable, body, old))
        translated_addresses = {int(item[0], 16) for item in functions}
        self.assertEqual(translated_addresses, retail_addresses)
        prelude = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c))exit(2);}while(0)
static uint8_t memory[0x20000];
static uint32_t eax,ecx,esi,esp,frees;
#define MEM32(a) (*(uint32_t *)(void *)(memory+(uint32_t)(a)))
#define MEM8(a) memory[(uint32_t)(a)]
#define TEST_Z(a,b) (((uint32_t)(a)&(uint32_t)(b))==0)
#define PUSH32(s,v) do {uint32_t v_=(v);(s)-=4;MEM32(s)=v_;}while(0)
#define POP32(s,v) do {(v)=MEM32(s);(s)+=4;}while(0)
#define RECOMP_TRACE_FUNC(a) ((void)0)
static void sub_001F6B60(void) {CHECK(MEM32(esp+4)==0x1000);++frees;eax=0xBAD;ecx=0;esp+=4;}
'''
        declarations = '\n'.join(f[2] for f in functions) + '\n' + '\n'.join(
            f[3].replace(f'void sub_{f[0]}(', f'void old_{f[0]}(') for f in functions)
        table = '\n'.join(f'{{sub_{name},old_{name},0x{vtable:X}}},' for name, vtable, *_ in functions)
        harness = r'''
typedef void (*routine)(void);
static struct {routine fixed,old;uint32_t vtable;} cases[]={TABLE};
static int check(routine f,uint32_t vtable,unsigned flag,unsigned ret,unsigned saved) {
 frees=0;esp=0x10000;ecx=0x1000;esi=saved;eax=0;
 MEM32(ecx)=0xDEADBEEF;PUSH32(esp,flag);PUSH32(esp,ret);f();
 CHECK(esp==0x10000&&esi==saved&&eax==0x1000&&MEM32(0x1000)==vtable);
 return frees==(flag&1);
}
int main(void) {
 unsigned total=0,negative=0;
 for(unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);++i) {
  unsigned failures=0;
  for(unsigned flag=0;flag<8;++flag)for(unsigned ret=0;ret<2;++ret)for(unsigned saved=0;saved<2;++saved) {
   CHECK(check(cases[i].fixed,cases[i].vtable,flag,0x123450+ret,0xABCDE0+saved));
   failures+=!check(cases[i].old,cases[i].vtable,flag,0x123450+ret,0xABCDE0+saved);++total;
  }
  CHECK(failures==16);++negative;
 }
 printf("%u fixed cases passed; %u old routines failed negative controls\n",total,negative);
 return 0;
}
'''.replace('TABLE', table)
        compiler = shutil.which('gcc') or 'C:/MinGW/bin/gcc.exe'
        with tempfile.TemporaryDirectory(prefix='mercs-scalar-delete-') as directory:
            c = Path(directory) / 'test.c'; exe = Path(directory) / 'test.exe'
            c.write_text(prelude + declarations + harness, encoding='utf-8')
            result = subprocess.run([compiler, '-std=c11', str(c), '-o', str(exe)], capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr.decode(errors='replace'))
            result = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            print(result.stdout.strip())


if __name__ == '__main__':
    unittest.main()
