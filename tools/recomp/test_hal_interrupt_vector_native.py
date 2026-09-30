"""Execute the production Xbox bus IRQ/vector/IRQL mapping and null handling."""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class InterruptVectorTests(unittest.TestCase):
    def test_mapping_and_bridge_null_pointer(self):
        source = (ROOT / 'src/kernel/kernel_hal.c').read_text(encoding='utf-8')
        body = re.search(r'ULONG __stdcall xbox_HalGetInterruptVector\([^;]+?\)\n\{.*?\n\}', source, re.S)[0]
        bridge = (ROOT / 'src/kernel/kernel_bridge.c').read_text(encoding='utf-8')
        self.assertIn('bus_level, irql_va ? (PKIRQL)XBOX_TO_NATIVE(irql_va) : NULL', bridge)
        prelude = '#include <stdint.h>\n#include <assert.h>\n#include <stddef.h>\ntypedef uint32_t ULONG;\ntypedef uint8_t KIRQL;\ntypedef KIRQL *PKIRQL;\n'
        harness = r'''
int main(void) {
 for (ULONG bus=0;bus<64;++bus) {
  KIRQL irql=0xa5;
  ULONG vector=xbox_HalGetInterruptVector(bus,&irql);
  assert(vector==xbox_HalGetInterruptVector(bus,NULL));
  if(bus<=27) { assert(vector==0x30+bus); assert(irql==27-bus); }
  else { assert(vector==0 && irql==0xa5); }
 }
 KIRQL apu=0;assert(xbox_HalGetInterruptVector(5,&apu)==0x35 && apu==22);
 KIRQL sentinel=0xa5;assert(!xbox_HalGetInterruptVector(UINT32_MAX,&sentinel)&&sentinel==0xa5);
 return 0;
}
'''
        compiler = shutil.which('gcc') or 'C:/MinGW/bin/gcc.exe'
        with tempfile.TemporaryDirectory(prefix='merc-hal-vector-') as temp:
            directory = Path(temp)
            for broken in (False, True):
                actual = body.replace('(KIRQL)(27u - BusInterruptLevel)', '(KIRQL)0') if broken else body
                c = directory/'test.c'; exe = directory/'test.exe'
                c.write_text(prelude+actual+harness, encoding='utf-8')
                subprocess.run([compiler,'-std=c11',str(c),'-o',str(exe)], check=True)
                result = subprocess.run([str(exe)], capture_output=True, timeout=10)
                if broken:
                    self.assertNotEqual(result.returncode,0)
                else:
                    self.assertEqual(result.returncode,0,result.stderr)
        print('64 bus levels, null outputs, overflow and APU priority passed; zero-IRQL regression rejected')


if __name__ == '__main__':
    unittest.main()
