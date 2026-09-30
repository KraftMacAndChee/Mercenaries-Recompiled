"""Verify Audio Options preserves untouched channel volumes while editing."""

from pathlib import Path
import runpy
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))


class AudioOptionsStateTests(unittest.TestCase):
    def test_entry_seeds_all_live_volume_fields(self) -> None:
        patcher = runpy.run_path(
            str(ROOT / "ports/mercenaries/scripts/Patch-Generated.py")
        )
        patch = next(
            item
            for item in patcher["PATCHES"]
            if item.name == "retail Audio Options initializes all live volume fields"
        )
        generated = (
            ROOT / "ports/mercenaries/src/recomp/gen/recomp_0004.c"
        ).read_text(encoding="utf-8")
        self.assertIn(patch.after, generated)

        for sfx in (0.0, 0.1, 0.5, 0.9, 1.0):
            for music in (0.0, 0.25, 0.75, 1.0):
                for dialogue in (0.0, 0.4, 1.0):
                    previous = [sfx, music, dialogue]
                    live = previous.copy()
                    live[1] = 0.6
                    self.assertEqual(live[0], sfx)
                    self.assertEqual(live[2], dialogue)


if __name__ == "__main__":
    unittest.main()
