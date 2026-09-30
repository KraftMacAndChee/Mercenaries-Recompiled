"""Verify each recovered straight-line shared epilogue against retail bytes/ABI."""
from pathlib import Path
import runpy
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from tools.recomp import config


class SharedEpilogueTests(unittest.TestCase):
    def test_translator_inlined_shape_is_recognized(self):
        module = runpy.run_path(str(ROOT / 'ports/mercenaries/scripts/Patch-Generated.py'))
        inline = module['TRANSLATOR_INLINED_PATCHES'][
            'Retail shared epilogue 000E1D98 restores its actual frame']
        self.assertEqual(inline, '''loc_000E1D98: ;
    POP32(esp, esi);
    esp = esp + 0x10;
    esp += 8; return; /* ret 4 */''')

    def test_all_retail_bytes_and_native_stack_frames(self):
        module = runpy.run_path(str(ROOT / 'ports/mercenaries/scripts/Patch-Generated.py'))
        entries = module['SIMPLE_SHARED_EPILOGUES']
        self.assertEqual(len(entries), 5)
        xbe = ROOT / 'game_files/mercenaries-retail/default.xbe'
        config.configure_from_xbe(str(xbe))
        raw = xbe.read_bytes()
        source = (ROOT / 'ports/mercenaries/src/recomp/gen/recomp_stubs_unresolved.c').read_text()
        cases, bodies = [], []
        for entry in entries:
            address, data, registers, local_size, arguments = entry
            offset = config.va_to_file_offset(int(address, 16))
            expected = bytes.fromhex(data)
            self.assertEqual(raw[offset:offset + len(expected)], expected, address)
            patch = module['simple_shared_epilogue_patch'](*entry)
            self.assertIn(patch.after, source)
            self.assertNotIn(patch.before, source)
            bodies += [patch.after, patch.before.replace(f'sub_{address}', f'old_{address}')]
            setup = '\n'.join(f'PUSH32(esp,0x12345000+{i});' for i in range(arguments // 4))
            setup += '\nPUSH32(esp,0x98765432);\n'
            setup += f'esp-=0x{local_size:X};\n'
            setup += '\n'.join(f'PUSH32(esp,{reg});' for reg in reversed(registers))
            cases.append(f'''for(unsigned i=0;i<16;++i) {{
 esp=0x10000;ebx=0x11110000+i;esi=0x22220000+i;edi=0x33330000+i;
 {setup}
 ebx=1;esi=2;edi=3;
 sub_{address}();
 CHECK(esp==0x10000);
 CHECK(ebx=={'0x11110000+i' if 'ebx' in registers else '1'});
 CHECK(esi=={'0x22220000+i' if 'esi' in registers else '2'});
 CHECK(edi=={'0x33330000+i' if 'edi' in registers else '3'});
 esp=0x10000;{setup}
 old_{address}();CHECK(esp!=0x10000);++cases;
}}''')
        prelude = r'''
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#define CHECK(c) do {if(!(c))exit(2);}while(0)
static uint8_t memory[0x20000];
static uint32_t esp,ebx,esi,edi;
#define MEM32(a) (*(uint32_t *)(void *)(memory+(uint32_t)(a)))
#define PUSH32(s,v) do{uint32_t v_=(v);(s)-=4;MEM32(s)=v_;}while(0)
#define POP32(s,v) do{(v)=MEM32(s);(s)+=4;}while(0)
'''
        harness = 'int main(void) {unsigned cases=0;\n' + '\n'.join(cases)
        harness += '\nprintf("%u shared-epilogue cases passed; all old stubs fail\\n",cases);return 0;}\n'
        with tempfile.TemporaryDirectory(prefix='merc-shared-ret-') as directory:
            path = Path(directory)
            (path / 'test.c').write_text(prelude + '\n'.join(bodies) + harness)
            exe = path / 'test.exe'
            subprocess.run([shutil.which('gcc') or 'C:/MinGW/bin/gcc.exe', '-O2', '-std=c11',
                            str(path / 'test.c'), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    unittest.main()
