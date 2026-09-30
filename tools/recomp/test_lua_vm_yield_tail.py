import runpy
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
PATCHER = ROOT / "ports/mercenaries/scripts/Patch-Generated.py"
GENERATED = (
    ROOT
    / "ports/mercenaries/src/recomp/gen/recomp_0009.c"
)


class LuaVmYieldTailTests(unittest.TestCase):
    def test_internal_yield_tail_matches_retail_callinfo_writes(self):
        patches = runpy.run_path(str(PATCHER))["PATCHES"]
        patch = next(p for p in patches if p.name == "luaV_execute internal yield tail")

        expected = (
            "ecx = MEM32(ebx + 0x14);\n"
            "        edx = MEM32(esp + 0x0C);\n"
            "        MEM32(ecx - 0x0C) = edx;\n"
            "        eax = MEM32(ebx + 0x14);\n"
            "        MEM32(eax - 0x10) = 8;\n"
            "        eax = 0;"
        )
        self.assertIn(expected, patch.after)
        self.assertIn(expected, GENERATED.read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()
