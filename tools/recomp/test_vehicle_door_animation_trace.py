"""Keep the vehicle-door observer reproducible and behavior-neutral by default."""

from pathlib import Path
import runpy
import unittest

from generated_test_utils import GEN

ROOT = Path(__file__).resolve().parents[2]


class VehicleDoorAnimationTraceTests(unittest.TestCase):
    def test_observer_is_not_injected_into_production_generated_code(self) -> None:
        generated = "\n".join(
            path.read_text(encoding="utf-8")
            for path in sorted(GEN.glob("recomp_*.c"))
        )
        self.assertNotIn("recomp_vehicle_door_checkpoint(", generated)

    def test_observer_is_environment_gated_and_has_no_state_writes(self) -> None:
        source = (ROOT / "ports/mercenaries/src/recomp_manual.c").read_text(
            encoding="utf-8"
        )
        start = source.index("void recomp_vehicle_door_checkpoint(")
        end = source.index("\nvoid recomp_human_head_checkpoint(", start)
        observer = source[start:end]
        self.assertIn("MERCENARIES_TRACE_VEHICLE_DOOR", observer)
        self.assertNotIn("guest_write", observer)
        self.assertNotIn("MEM32", observer)


if __name__ == "__main__":
    unittest.main()
