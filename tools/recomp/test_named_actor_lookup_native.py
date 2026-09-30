"""Exercise the production actor lookup against the observed retail heap layout."""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
MANUAL = ROOT / "ports/mercenaries/src/recomp_manual.c"

PRELUDE = r"""
#include <assert.h>
#include <stdint.h>
#include <string.h>
static uint8_t memory[0x04000000];
static uint32_t guest_u32(uint32_t address) {
    uint32_t result;
    assert(address <= sizeof(memory) - sizeof(result));
    memcpy(&result, memory + address, sizeof(result));
    return result;
}
static void set_word(uint32_t address, uint32_t value) {
    memcpy(memory + address, &value, sizeof(value));
}
"""

HARNESS = r"""
int main(void) {
    const uint32_t actor = 0x014BC000u;
    const uint32_t name = 0x9A21CF9Fu;
    /* Exact layout observed in hidden retail run 482: the preceding allocation
       also starts with a game vtable. The old +0x34 scan returns actor-0x30. */
    set_word(actor - 0x30u, 0x002DEEE4u);
    set_word(actor - 0x2Cu, 0x7CAD834Du);
    set_word(actor, 0x002E2CE0u);
    set_word(actor + 4u, name);
    set_word(actor + 8u, 0x005558B8u);
    set_word(actor + 0x34u, actor);
    assert(recomp_find_named_actor(name) == actor);
    assert(recomp_find_named_actor(0u) == 0u);
    assert(recomp_find_named_actor(0x12345678u) == 0u);
    set_word(actor, 0u);
    assert(recomp_find_named_actor(name) == 0u);
    /* The cached object may disappear/reuse memory between polling frames. */
    set_word(actor, 0x002E2CE0u);
    set_word(actor + 4u, 0x12345678u);
    assert(recomp_find_named_actor(name) == 0u);
    assert(recomp_find_named_actor(0x12345678u) == actor);
    return 0;
}
"""


class NamedActorLookupTests(unittest.TestCase):
    def test_authored_names_use_correct_hashes(self):
        source = MANUAL.read_text(encoding="utf-8")
        for name in ("alliesbouncer", "starter_trigger", "clubs2"):
            value = 2166136261
            for byte in name.encode("ascii"):
                value = ((value ^ (byte | 0x20)) * 16777619) & 0xFFFFFFFF
            self.assertIn(f"recomp_find_named_actor(0x{value:08X}u)", source)
            if name == "starter_trigger":
                self.assertEqual(value, 0x7D454737)
                self.assertNotEqual(value, 0x8373EF17)  # ordinary lowercase is wrong

    def test_real_lookup_and_wrong_offset_negative_control(self):
        compiler = shutil.which("gcc") or shutil.which("clang")
        if compiler is None and Path("C:/MinGW/bin/gcc.exe").exists():
            compiler = "C:/MinGW/bin/gcc.exe"
        if compiler is None:
            self.skipTest("native actor lookup regression requires GCC or Clang")
        source = MANUAL.read_text(encoding="utf-8")
        # Other diagnostic helpers now follow this function. Compile only the
        # actor lookup under test, not unrelated controller/file-I/O code.
        production = re.search(
            r'static uint32_t recomp_find_named_actor\(.*?\n\}', source, re.S,
        )[0]
        for mutation in (False, True):
            with self.subTest(wrong_offset=mutation), tempfile.TemporaryDirectory(prefix="actor-native-") as temp:
                body = production.replace("guest_u32(actor + 4u)",
                                          "guest_u32(actor + 0x34u)") if mutation else production
                test_source = Path(temp) / "actor-test.c"
                executable = Path(temp) / "actor-test.exe"
                test_source.write_text(PRELUDE + body + HARNESS, encoding="utf-8")
                build = subprocess.run([compiler, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                                str(test_source), "-o", str(executable)],
                               capture_output=True, text=True, timeout=60)
                self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
                result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=10)
                if mutation:
                    self.assertNotEqual(result.returncode, 0, "Old offset must fail the observed-layout test")
                else:
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=2)
