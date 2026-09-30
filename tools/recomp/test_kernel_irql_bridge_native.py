"""Execute real IRQL bridges with poisoned stack arguments and nested levels."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class IRQLBridgeTests(unittest.TestCase):
    def test_fastcall_and_guest_kpcr(self):
        source = (ROOT / 'src/kernel/kernel_bridge.c').read_text(encoding='utf-8')
        bodies = []
        for name in ('KfRaiseIrql', 'KfLowerIrql', 'KeRaiseIrqlToDpcLevel'):
            start = source.index(f'static void bridge_{name}(void)')
            bodies.append(source[start:source.index('\n}', start) + 2])
        prelude = r'''
#include <assert.h>
#include <stdint.h>
typedef uint8_t UCHAR;
#define DISPATCH_LEVEL 2u
static uint32_t g_eax, g_ecx, stack_poison = 0xFE;
static uint8_t memory[64], level;
#define BRIDGE_MEM8(p) memory[p]
#define STACK_ARG(i) stack_poison
static UCHAR xbox_KfRaiseIrql(UCHAR value) { UCHAR old = level; level = value; return old; }
static void xbox_KfLowerIrql(UCHAR value) { level = value; }
static UCHAR xbox_KeRaiseIrqlToDpcLevel(void) { return xbox_KfRaiseIrql(2); }
'''
        harness = r'''
int main(void) {
    g_ecx = 0xCAFEBE02; bridge_KfRaiseIrql();
    assert(g_eax == 0 && level == 2 && memory[0x24] == 2);
    g_ecx = 3; bridge_KfRaiseIrql();
    assert(g_eax == 2 && level == 3 && memory[0x24] == 3);
    g_ecx = 0xDEAD0002; bridge_KfLowerIrql();
    assert(level == 2 && memory[0x24] == 2);
    g_ecx = 0; bridge_KfLowerIrql();
    assert(level == 0 && memory[0x24] == 0);
    bridge_KeRaiseIrqlToDpcLevel();
    assert(g_eax == 0 && level == 2 && memory[0x24] == 2);
    return 0;
}
'''
        compiler = shutil.which('gcc') or 'C:/MinGW/bin/gcc.exe'
        with tempfile.TemporaryDirectory(prefix='merc-irql-') as temp:
            path = Path(temp)
            for mode in ('fixed', 'old'):
                code = '\n'.join(bodies)
                if mode == 'old':
                    code = code.replace('g_ecx & 0xFFu', 'STACK_ARG(0)')
                (path / 'fixture.c').write_text(prelude + code + harness, encoding='utf-8')
                subprocess.run([compiler, str(path / 'fixture.c'), '-o', str(path / 'fixture.exe')], check=True)
                result = subprocess.run([str(path / 'fixture.exe')], capture_output=True)
                if mode == 'fixed':
                    self.assertEqual(result.returncode, 0, result.stderr)
                else:
                    self.assertNotEqual(result.returncode, 0)


if __name__ == '__main__':
    unittest.main()
