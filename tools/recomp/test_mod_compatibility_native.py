"""Read real per-executable INI settings with independent mod capacity flags."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[2]
CC = Path("C:/msys64/mingw64/bin/gcc.exe")

class ModCompatibilityTests(unittest.TestCase):
    def test_independent_flags_defaults_and_executable_relative_path(self):
        source = r'''
#include <assert.h>
#include <stdlib.h>
#include "kernel/xbox_memory_layout.h"
#include "mod_compatibility.h"
static int graphics = -1;
void xbox_SetExtendedGraphicsMemory(BOOL enabled) { graphics = enabled; }
int main(int argc, char **argv) {
    assert(argc == 3);
    recomp_mod_compatibility_init();
    assert(graphics == atoi(argv[1]));
    assert(recomp_mod_expanded_world_properties() == atoi(argv[2]));
    return 0;
}
'''
        env = os.environ.copy(); env["PATH"] = str(CC.parent) + os.pathsep + env["PATH"]
        with tempfile.TemporaryDirectory(prefix="merc-mod-config-") as tmp:
            root = Path(tmp); exe = root / "test.exe"; src = root / "test.c"
            src.write_text(source, encoding="utf-8")
            port = ROOT / "ports/mercenaries/src"
            result = subprocess.run([str(CC), "-std=c11", "-O2", "-include", "windows.h", "-I", str(ROOT / "src"), "-I", str(port), str(src), str(port / "mod_compatibility.c"), "-o", str(exe)], env=env, capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stderr)
            cwd = root / "different-cwd"; cwd.mkdir()
            (cwd / "modcompatibility.ini").write_text("[Mods]\nextended_graphics_memory=1\nexpanded_world_properties=1\n")
            (root / "compatibility.ini").write_text("[Graphics]\nextended_memory=1\n")
            cases = [(None, 0, 0), ("[Mods]\n", 0, 0)]
            for g in range(2):
                for w in range(2):
                    cases.append((f"[Mods]\nextended_graphics_memory={g}\nexpanded_world_properties={w}\n", g, w))
            cases.append(("[Mods]\nextended_graphics_memory=2\nexpanded_world_properties=invalid\n", 0, 0))
            for content, graphics, properties in cases:
                if content is not None: (root / "modcompatibility.ini").write_text(content)
                result = subprocess.run([str(exe), str(graphics), str(properties)], cwd=cwd, env=env, capture_output=True, text=True, timeout=10)
                self.assertEqual(result.returncode, 0, (content, result.stderr))

if __name__ == "__main__": unittest.main()
