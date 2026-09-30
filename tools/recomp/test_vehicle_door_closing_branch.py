"""Verify both authored vehicle-door orientations close over multiple ticks."""

from pathlib import Path
import runpy
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from tools.recomp import config
from generated_test_utils import generated_text_containing


class VehicleDoorClosingBranchTests(unittest.TestCase):
    def test_verified_retail_branch_and_generated_repair(self) -> None:
        xbe = ROOT / "game_files/mercenaries-retail/default.xbe"
        config.configure_from_xbe(str(xbe))
        raw = xbe.read_bytes()
        offset = config.va_to_file_offset(0x0016D224)
        expected = bytes.fromhex(
            "0f2fc18b4604760af30f1040740f2fc1eb040f2f48747214"
        )
        self.assertEqual(raw[offset:offset + len(expected)], expected)

        patcher = runpy.run_path(
            str(ROOT / "ports/mercenaries/scripts/Patch-Generated.py")
        )
        patch = next(
            item for item in patcher["PATCHES"]
            if item.name ==
            "retail vehicle door closing preserves branch-local COMISS operands"
        )
        generated = generated_text_containing(
            "0x0016D224 and 0x0016D236 reach the shared JB"
        )
        self.assertIn(patch.after, generated)

    def test_left_and_right_doors_do_not_snap_on_first_tick(self) -> None:
        def close(angle: float, speed: float, dt: float) -> tuple[float, bool]:
            angle += speed * dt
            reached = angle >= 0.0 if speed > 0.0 else angle <= 0.0
            return (0.0, True) if reached else (angle, False)

        left, left_closed = close(-75.0, 245.0, 1.0 / 30.0)
        right, right_closed = close(75.0, -245.0, 1.0 / 30.0)
        self.assertAlmostEqual(left, -66.8333333333)
        self.assertAlmostEqual(right, 66.8333333333)
        self.assertFalse(left_closed)
        self.assertFalse(right_closed)

        for _ in range(20):
            left, left_closed = close(left, 245.0, 1.0 / 30.0)
            right, right_closed = close(right, -245.0, 1.0 / 30.0)
        self.assertEqual((left, left_closed), (0.0, True))
        self.assertEqual((right, right_closed), (0.0, True))


if __name__ == "__main__":
    unittest.main()
