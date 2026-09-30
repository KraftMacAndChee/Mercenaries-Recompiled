"""Execute the real retail DirectSound status query, including its POP/TEST bug."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "ports/mercenaries/src/recomp/gen/recomp_0013.c"

PRELUDE = r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
static uint8_t memory[0x300000];
static uint32_t eax, ebx, ecx, edx, esi, esp, g_seh_ebp;
#define MEM32(a) (*(uint32_t *)(void *)(memory + (uint32_t)(a)))
#define MEM8(a) memory[(uint32_t)(a)]
#define MEM16(a) (*(uint16_t *)(void *)(memory + (uint32_t)(a)))
#define PUSH32(s,v) do { uint32_t value_ = (v); (s) -= 4; MEM32(s) = value_; } while (0)
#define POP32(s,v) do { (v) = MEM32(s); (s) += 4; } while (0)
#define CMP_EQ(a,b) ((uint32_t)(a) == (uint32_t)(b))
#define CMP_NE(a,b) ((uint32_t)(a) != (uint32_t)(b))
#define CMP_AE(a,b) ((uint32_t)(a) >= (uint32_t)(b))
#define TEST_Z(a,b) (((uint32_t)(a) & (uint32_t)(b)) == 0)
#define TEST_NZ(a,b) (!TEST_Z(a,b))
#define ZX8(a) ((uint32_t)(uint8_t)(a))
#define ZX16(a) ((uint32_t)(uint16_t)(a))
#define LO8(a) ((uint8_t)(a))
#define RECOMP_TRACE_FUNC(a) ((void)0)
'''

HARNESS = r'''
static void check(uint32_t parent_flags, uint32_t caller_flags,
                  uint32_t own_flags, uint32_t link, uint32_t expected_status,
                  uint32_t expected_link, int has_parent) {
    memset(memory, 0, sizeof(memory));
    ecx = 0x1000; esi = 0x7000; esp = 0x1F000;
    ebx = 0xABCD; g_seh_ebp = 0x123456;
    MEM32(ecx + 0x70) = 0x2000;
    MEM32(ecx + 8) = 0x5000;
    MEM32(0x2000 + 8) = own_flags;
    MEM32(0x2000 + 0xB0) = has_parent ? 0x3500 : 0;
    MEM32(0x3500 + 0x20) = 0x3000;
    MEM32(0x3000 + 0x70) = 0x4000;
    MEM32(0x3000 + 0x50) = link;
    MEM32(0x4000 + 8) = parent_flags;
    MEM32(esi + 8) = caller_flags;
    MEM32(0x8000) = 0xFFFFFFFF; MEM32(0x8004) = 0xFFFFFFFF;
    PUSH32(esp, 0x8004); PUSH32(esp, 0x8000); PUSH32(esp, 0);
    sub_002A35E7();
    assert(MEM8(0x8000) == expected_status);
    assert(MEM32(0x8004) == expected_link);
    assert(esp == 0x1F000 && esi == 0x7000 && ebx == 0xABCD);
    assert(g_seh_ebp == 0x123456);
}
int main(void) {
    check(0x82000, 0, 0, 0x6000, 2, 0x6000 - 0x4C, 1);
    check(0, 0x82000, 0, 0x6000, 0, 0, 1);
    check(0x82000, 0, 0, 0x5498, 2, 0, 1);
    check(0, 0, 0x182000, 0x6000, 2, 0, 1);
    check(0, 0, 0x200010, 0x6000, 1, 0, 1);
    check(0x82000, 0x82000, 0, 0x6000, 0, 0, 0);
    return 0;
}
'''


class DirectSoundParentStatusTests(unittest.TestCase):
    def test_hardware_voice_check_and_negative_control(self):
        compiler = shutil.which("gcc") or "C:/MinGW/bin/gcc.exe"
        if not Path(compiler).exists():
            self.skipTest("GCC required for executable regression")
        text = SOURCE.read_text(encoding="utf-8")
        start = text.index("void sub_002A120C(void)")
        function = text[start:text.index("\n}\n", start) + 3]
        snapshot = "    _flags = (TEST_NZ(MEM32(edx + esi + 4), 0x800000)); /* preserve test flags across 1 instruction(s) */\n"
        self.assertIn(snapshot, function)
        old = function.replace(snapshot, "").replace(
            "if (_flags != 0) goto loc_002A1259;",
            "if (TEST_NZ(MEM32(edx + esi + 4), 0x800000)) goto loc_002A1259;")
        stub = r'''
static unsigned callbacks;
static void sub_002A365B(void) {
    assert(ecx == 0x5000 && MEM32(esp + 4) == 1);
    ++callbacks; esp += 8;
}
'''
        harness = r'''
static void check(uint32_t hardware, uint32_t caller, int expected,
                  uint32_t selected, int self_link, int disabled) {
    memset(memory, 0, sizeof(memory)); callbacks = 0;
    ecx = 0x1000; esi = 0x7000; esp = 0x1F000;
    MEM32(0x2BB1A0) = 0x4000;
    MEM32(0x4000 + (3 << 7) + 4) = hardware;
    MEM32(esi + (3 << 7) + 4) = caller;
    MEM32(ecx + 0x84) = disabled;
    MEM32(ecx + 3 * 4 + 0x88) = 0x5000;
    MEM8(0x5064) = 0;
    MEM16(0x500A) = selected;
    MEM32(0x504C) = self_link ? 0x504C : 0x6000;
    PUSH32(esp, 3); PUSH32(esp, 0);
    sub_002A120C();
    assert(callbacks == (unsigned)expected);
    assert(esp == 0x1F000 && esi == 0x7000);
}
int main(void) {
    check(0, 0x800000, 1, 3, 0, 0);
    check(0x800000, 0, 0, 3, 0, 0);
    check(0, 0, 0, 2, 0, 0);
    check(0, 0, 0, 3, 1, 0);
    check(0, 0, 0, 3, 0, 1);
    return 0;
}
'''
        for name, code in (("fixed", function), ("old", old)):
            with self.subTest(name=name), tempfile.TemporaryDirectory(prefix="mercs-dsound-hw-") as directory:
                source = Path(directory) / "status.c"
                executable = Path(directory) / "status.exe"
                source.write_text(PRELUDE + stub + code + harness, encoding="utf-8")
                subprocess.run([compiler, "-std=c11", str(source), "-o", str(executable)], check=True, capture_output=True)
                result = subprocess.run([str(executable)], capture_output=True)
                if name == "fixed":
                    self.assertEqual(result.returncode, 0, result.stderr.decode(errors="replace"))
                else:
                    self.assertNotEqual(result.returncode, 0, "Old hardware-address bug escaped detection")

    def test_generated_function_and_negative_control(self):
        compiler = shutil.which("gcc") or "C:/MinGW/bin/gcc.exe"
        if not Path(compiler).exists():
            self.skipTest("GCC required for executable regression")
        text = SOURCE.read_text(encoding="utf-8")
        start = text.index("void sub_002A35E7(void)")
        end = text.index("\n}\n", start) + 3
        function = text[start:end]
        snapshot = "    _flags = (TEST_Z(MEM32(esi + 8), 0x82000)); /* preserve test flags across 1 instruction(s) */\n"
        self.assertIn(snapshot, function)
        old = function.replace(snapshot, "").replace(
            "if (_flags != 0) goto loc_002A362E;",
            "if (TEST_Z(MEM32(esi + 8), 0x82000)) goto loc_002A362E;")
        for name, code, expected in (("fixed", function, 0), ("old", old, None)):
            with self.subTest(name=name), tempfile.TemporaryDirectory(prefix="mercs-dsound-") as directory:
                source = Path(directory) / "status.c"
                executable = Path(directory) / "status.exe"
                source.write_text(PRELUDE + code + HARNESS, encoding="utf-8")
                subprocess.run([compiler, "-std=c11", str(source), "-o", str(executable)], check=True, capture_output=True)
                result = subprocess.run([str(executable)], capture_output=True)
                if expected == 0:
                    self.assertEqual(result.returncode, 0, result.stderr.decode(errors="replace"))
                else:
                    self.assertNotEqual(result.returncode, 0, "Old address-register bug escaped detection")


if __name__ == "__main__":
    unittest.main(verbosity=2)
