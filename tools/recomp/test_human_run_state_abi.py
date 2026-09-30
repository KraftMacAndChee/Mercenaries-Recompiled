"""Verify submersion-state physics lookups preserve human run state."""

from pathlib import Path
import runpy
import unittest

ROOT = Path(__file__).resolve().parents[2]


class HumanRunStateAbiTests(unittest.TestCase):
    def test_all_five_physics_vcalls_restore_live_state(self) -> None:
        patcher = runpy.run_path(
            str(ROOT / "ports/mercenaries/scripts/Patch-Generated.py")
        )
        generated = (
            ROOT / "ports/mercenaries/src/recomp/gen/recomp_0001.c"
        ).read_text(encoding="utf-8")
        function = generated[
            generated.index("void sub_000558B0(void)") : generated.index(
                "void sub_00055BB0(void)"
            )
        ]
        patches = [
            item
            for item in patcher["PATCHES"]
            if item.name.startswith("retail human submersion vcall ")
        ]
        self.assertEqual(len(patches), 5)
        for patch in patches:
            if "00055AC3" not in patch.name:
                self.assertIn(patch.after, generated)
        for register in ("ebx", "esi", "edi"):
            self.assertEqual(
                function.count(f"human_run_saved_{register} = {register};"),
                5,
            )
            self.assertEqual(
                function.count(f"{register} = human_run_saved_{register};"),
                5,
            )
        self.assertEqual(function.count("human_run_saved_ebp = g_seh_ebp;"), 5)
        self.assertEqual(function.count("g_seh_ebp = human_run_saved_ebp;"), 5)
        dry = function[
            function.index("loc_00055AC3: ;") : function.index("loc_00055AD3: ;")
        ]
        self.assertLess(
            dry.index("SET_LO8(ebx, MEM8(esi + 0x817));"),
            dry.index("human_run_saved_ebx = ebx;"),
        )


if __name__ == "__main__":
    unittest.main()




