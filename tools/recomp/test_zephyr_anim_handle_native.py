"""Verify opt-in Zephyr animation-handle integrity tracing stays wired."""

from pathlib import Path
import runpy
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))


class ZephyrAnimationHandleTests(unittest.TestCase):
    def test_setter_and_sampler_checkpoints_are_generated(self) -> None:
        patcher = runpy.run_path(
            str(ROOT / "ports/mercenaries/scripts/Patch-Generated.py")
        )
        patches = {item.name: item for item in patcher["PATCHES"]}
        generated = "\n".join(
            path.read_text(encoding="utf-8")
            for path in (ROOT / "ports/mercenaries/src/recomp/gen").glob(
                "recomp_*.c"
            )
        )
        for name in (
            "retail Zephyr animation handle setter trace",
            "retail Zephyr animation sampler handle trace",
        ):
            self.assertIn(patches[name].after, generated)

    def test_checkpoint_uses_retail_zephyr_layout_bounds(self) -> None:
        manual = (ROOT / "ports/mercenaries/src/recomp_manual.c").read_text(
            encoding="utf-8"
        )
        self.assertIn("instance + 0xB00u", manual)
        self.assertIn("joints > 0u && joints <= 64u", manual)
        self.assertIn("(joint_data & 3u) == 0u", manual)
        runner = (ROOT / "tools/recomp/Run-HiddenRetailManual.ps1").read_text(
            encoding="utf-8"
        )
        self.assertIn("[switch]$TraceZephyrAnim", runner)
        self.assertIn("MERCENARIES_TRACE_ZEPHYR_ANIM", runner)


if __name__ == "__main__":
    unittest.main()