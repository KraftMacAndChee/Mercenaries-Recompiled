"""Verify static-runtime hardware IRQs preserve level-triggered semantics."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class HardwareIrqLevelTests(unittest.TestCase):
    def test_deasserted_level_is_not_delivered_as_a_stale_edge(self):
        source = (ROOT / "src/kernel/kernel_bridge.c").read_text(encoding="utf-8")
        bodies = []
        for name in (
            "xbox_kernel_raise_hardware_interrupt",
            "xbox_kernel_lower_hardware_interrupt",
        ):
            start = source.index(f"void {name}(")
            bodies.append(source[start : source.index("\n}", start) + 2])

        self.assertNotIn("g_kernel_asserted_hardware_interrupts", source)
        delivery = source.split("static int bridge_deliver_hardware_interrupts", 1)[1]
        delivery = delivery.split("void xbox_kernel_service_hardware_interrupts", 1)[0]
        self.assertNotIn("re-latch", delivery)
        self.assertNotIn("g_kernel_asserted_hardware_interrupts", delivery)

        prelude = r'''
#include <assert.h>
#include <stdint.h>
typedef int32_t LONG;
static volatile int32_t g_kernel_pending_hardware_interrupts;
static LONG InterlockedOr(volatile LONG *p, LONG v) { LONG old=*p; *p|=v; return old; }
static LONG InterlockedAnd(volatile LONG *p, LONG v) { LONG old=*p; *p&=v; return old; }
'''
        harness = r'''
int main(void) {
    xbox_kernel_raise_hardware_interrupt(5);
    assert(g_kernel_pending_hardware_interrupts == (1 << 5));
    xbox_kernel_lower_hardware_interrupt(5);
    assert(g_kernel_pending_hardware_interrupts == 0);

    xbox_kernel_raise_hardware_interrupt(5);
    xbox_kernel_raise_hardware_interrupt(7);
    xbox_kernel_lower_hardware_interrupt(5);
    assert(g_kernel_pending_hardware_interrupts == (1 << 7));
    xbox_kernel_lower_hardware_interrupt(7);
    assert(g_kernel_pending_hardware_interrupts == 0);

    xbox_kernel_raise_hardware_interrupt(32);
    assert(g_kernel_pending_hardware_interrupts == 0);
    return 0;
}
'''
        fixed = prelude + "\n".join(bodies) + harness
        stale_edge = fixed.replace(
            "InterlockedAnd((volatile LONG *)&g_kernel_pending_hardware_interrupts,\n"
            "                       (LONG)~(1u << bus_level));",
            "(void)bus_level;",
        )
        self.assertNotEqual(fixed, stale_edge)
        compiler = shutil.which("gcc") or "C:/MinGW/bin/gcc.exe"
        with tempfile.TemporaryDirectory(prefix="merc-hw-irq-level-") as directory:
            path = Path(directory)
            for label, code in (("fixed", fixed), ("stale-edge", stale_edge)):
                source_path = path / f"{label}.c"
                exe_path = path / f"{label}.exe"
                source_path.write_text(code, encoding="utf-8")
                subprocess.run(
                    [compiler, "-std=c11", str(source_path), "-o", str(exe_path)],
                    check=True,
                )
                result = subprocess.run([str(exe_path)], capture_output=True)
                if label == "fixed":
                    self.assertEqual(result.returncode, 0, result.stderr)
                else:
                    self.assertNotEqual(result.returncode, 0)


if __name__ == "__main__":
    unittest.main()
