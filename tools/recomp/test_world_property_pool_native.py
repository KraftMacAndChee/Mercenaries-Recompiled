"""Check mod world-property storage and its generated-code integration."""
import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
CC = Path("C:/msys64/mingw64/bin/gcc.exe")


class WorldPropertyPoolTests(unittest.TestCase):
    def test_storage_contents_and_bounds(self):
        source = r'''
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
static unsigned extended, allocations;
int recomp_mod_expanded_world_properties(void) { return extended != 0; }
uint32_t xbox_HeapAlloc(uint32_t size, uint32_t alignment) {
    assert(size == 262144u * 8u && alignment == 16u);
    ++allocations;
    return extended == 2 ? 0 : 0x01000000u;
}
'''
        source += '#include "' + (ROOT / "ports/mercenaries/src/recomp_world_properties.c").as_posix() + '"\n'
        source += r'''
int main(int argc, char **argv) {
    assert(argc == 2);
    unsigned mode = (unsigned)atoi(argv[1]);
    extended = mode >= 3;
    if (mode == 7) extended = 2;
    if (mode == 1 || mode == 4) {
        recomp_world_property_check_write(recomp_world_property_address(extended ? 262144u : 40000u));
        return 1;
    }
    if (mode == 2 || mode == 5) {
        recomp_world_property_address(extended ? 262145u : 40001u);
        return 1;
    }
    if (mode == 6) {
        recomp_world_property_check_write(recomp_world_property_address(0) + 1);
        return 1;
    }
    uint32_t base = recomp_world_property_address(0);
    if (mode == 7) return 1;
    uint32_t count = extended ? 262144u : 40000u;
    assert(base == (extended ? 0x01000000u : 0x004434E0u));
    unsigned char *memory = calloc(1, 0x01400000u);
    assert(memory);
    memset(memory + 0x4916E0u, 0xA5, 240000);
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t a = recomp_world_property_address(i);
        recomp_world_property_check_write(a);
        uint32_t pair[2] = {i, i ^ 0xDEADBEEFu};
        memcpy(memory + a, pair, 8);
    }
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t pair[2];
        memcpy(pair, memory + recomp_world_property_address(i), 8);
        assert(pair[0] == i && pair[1] == (i ^ 0xDEADBEEFu));
    }
    for (uint32_t i = 0; i < 240000; ++i) assert(memory[0x4916E0u + i] == 0xA5);
    /* Map reloads reuse the pool from index zero without allocating again. */
    assert(recomp_world_property_address(0) == base);
    assert(allocations == (extended ? 1u : 0u));
    recomp_world_property_address(count); /* Empty final object is legal. */
    free(memory);
    return 0;
}
'''
        env = os.environ.copy()
        env["PATH"] = str(CC.parent) + os.pathsep + env["PATH"]
        with tempfile.TemporaryDirectory(prefix="mercs-properties-") as tmp:
            src, exe = Path(tmp) / "test.c", Path(tmp) / "test.exe"
            src.write_text(source, encoding="utf-8")
            result = subprocess.run([str(CC), "-std=c11", "-O2", "-I", str(ROOT / "src"), str(src), "-o", str(exe)], env=env, capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stderr)
            for mode in range(8):
                result = subprocess.run([str(exe), str(mode)], env=env, capture_output=True, text=True, timeout=60)
                self.assertEqual(result.returncode, 0 if mode in (0, 3) else 86, result.stdout + result.stderr)

    def test_all_generated_property_bases_are_patched(self):
        path = ROOT / "ports/mercenaries/scripts/Patch-Generated.py"
        spec = importlib.util.spec_from_file_location("property_patches", path)
        module = importlib.util.module_from_spec(spec)
        sys.modules[spec.name] = module
        spec.loader.exec_module(module)
        patches = [p for p in module.PATCHES if p.name.startswith(("Mod permanent-property address", "Permanent-property write bounds"))]
        self.assertEqual(len(patches), 4)
        source = (ROOT / "ports/mercenaries/src/recomp/gen/recomp_0009.c").read_text(encoding="utf-8")
        original = source
        for patch in patches:
            self.assertEqual(source.count(patch.after), 1, patch.name)
            source = source.replace(patch.after, patch.before)
        self.assertEqual(source.count("eax * 8 + 0x4434E0"), 3)
        for patch in patches:
            self.assertEqual(source.count(patch.before), 1, patch.name)
            source = source.replace(patch.before, patch.after, 1)
        self.assertEqual(source, original)
        self.assertNotIn("eax * 8 + 0x4434E0", source)


if __name__ == "__main__":
    unittest.main()
