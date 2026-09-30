"""Verify the actor animation update preserves guest nonvolatile registers."""

from pathlib import Path
import runpy
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))


class ActorUpdateAbiTests(unittest.TestCase):
    def test_animation_update_call_restores_nonvolatile_registers(self) -> None:
        patcher = runpy.run_path(
            str(ROOT / "ports/mercenaries/scripts/Patch-Generated.py")
        )
        patch = next(
            item
            for item in patcher["PATCHES"]
            if item.name
            == "retail actor animation update preserves nonvolatile registers"
        )
        generated = (
            ROOT / "ports/mercenaries/src/recomp/gen/recomp_0001.c"
        ).read_text(encoding="utf-8")
        self.assertIn(patch.after, generated)
        self.assertIn("_actor_update_saved_ebx", patch.after)
        self.assertIn("_actor_update_saved_esi", patch.after)
        self.assertIn("_actor_update_saved_edi", patch.after)
        self.assertIn("_actor_update_saved_ebp", patch.after)
        self.assertIn("recomp_update_esi_checkpoint(0x000471C0u", patch.after)


if __name__ == "__main__":
    unittest.main()
