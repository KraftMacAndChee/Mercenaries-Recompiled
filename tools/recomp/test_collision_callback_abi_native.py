"""Verify retail collision callback objects survive generated virtual calls."""

from pathlib import Path
import runpy
import unittest

from generated_test_utils import generated_text_containing

ROOT = Path(__file__).resolve().parents[2]


class CollisionCallbackAbiTests(unittest.TestCase):
    def test_setter_and_consumer_restore_preserved_objects(self) -> None:
        patcher = runpy.run_path(
            str(ROOT / "ports/mercenaries/scripts/Patch-Generated.py")
        )
        patches = {item.name: item for item in patcher["PATCHES"]}
        generated = "\n".join((
            generated_text_containing("collision_callback_target = esi;"),
            generated_text_containing("collision_callback_object = esi;"),
        ))
        names = (
            "retail collision callback setter declares preserved target",
            "retail collision callback setter preserves target across vcall",
            "retail collision callback setter restores target after vcall",
            "retail collision callback consumer declares preserved object",
            "retail collision callback consumer preserves object across vcall",
            "retail collision callback consumer restores object after vcall",
            "retail collision callback setter snapshots all nonvolatiles",
            "retail collision callback setter restores all nonvolatiles",
            "retail collision callback consumer snapshots all nonvolatiles",
            "retail collision callback consumer restores all nonvolatiles",
        )
        superseded_declarations = {
            "retail collision callback setter declares preserved target",
            "retail collision callback consumer declares preserved object",
            # The source-controlled vehicle-door observer composes inside this
            # block after the ABI patch has run, so validate its invariants
            # below instead of requiring the pre-observer text verbatim.
            "retail collision callback consumer preserves object across vcall",
        }
        for name in names:
            if name in superseded_declarations:
                continue
            self.assertIn(patches[name].after, generated)
        self.assertIn("esi = collision_callback_target", generated)
        self.assertIn("esi = collision_callback_object", generated)
        self.assertIn("collision_callback_object = esi;", generated)
        self.assertIn("eax = recomp_collision_dispatch_state(esi, eax);", generated)
        for register in ("ebx", "esi", "edi"):
            self.assertIn(
                f"{register} = collision_callback_saved_{register};",
                generated,
            )
        self.assertGreaterEqual(
            generated.count("g_seh_ebp = collision_callback_saved_ebp;"),
            2,
        )


if __name__ == "__main__":
    unittest.main()
