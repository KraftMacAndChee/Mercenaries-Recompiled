"""Verify actor-list gathering preserves its live loop registers."""

from pathlib import Path
import runpy
import unittest

ROOT = Path(__file__).resolve().parents[2]


class ActorGatherAbiTests(unittest.TestCase):
    def test_all_five_predicate_vcalls_restore_loop_state(self) -> None:
        patcher = runpy.run_path(
            str(ROOT / "ports/mercenaries/scripts/Patch-Generated.py")
        )
        generated = (
            ROOT / "ports/mercenaries/src/recomp/gen/recomp_0004.c"
        ).read_text(encoding="utf-8")
        function = generated[
            generated.index("void sub_001066B0(void)") : generated.index(
                "void sub_001067B0(void)"
            )
        ]
        patches = [
            item
            for item in patcher["PATCHES"]
            if item.name.startswith("retail actor gather vcall ")
        ]
        self.assertEqual(len(patches), 5)
        for patch in patches:
            self.assertIn(patch.after, generated)
        for register in ("ebx", "esi", "edi", "ebp"):
            self.assertEqual(
                function.count(f"actor_gather_saved_{register} = {register};"),
                5,
            )
            self.assertEqual(
                function.count(f"{register} = actor_gather_saved_{register};"),
                5,
            )


if __name__ == "__main__":
    unittest.main()