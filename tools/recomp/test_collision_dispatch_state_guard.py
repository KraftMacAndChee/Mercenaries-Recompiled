from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]


class CollisionDispatchStateGuardTests(unittest.TestCase):
    def test_rejects_non_embedded_state_pointers(self):
        manual = (ROOT / "ports/mercenaries/src/recomp_manual.c").read_text()
        generated = (ROOT / "ports/mercenaries/src/recomp/gen/recomp_0007.c").read_text()
        patcher = (ROOT / "ports/mercenaries/scripts/Patch-Generated.py").read_text()

        self.assertIn("uint32_t recomp_collision_dispatch_state", manual)
        self.assertIn("state == object + 8u", manual)
        self.assertIn("state == object + 0x10u", manual)
        self.assertIn("state == object + 0x18u", manual)
        self.assertIn("[COLLISION-DISPATCH-REJECT]", manual)
        self.assertIn("return 0u;", manual)
        self.assertIn("eax = recomp_collision_dispatch_state(esi, eax);", generated)
        self.assertIn("retail collision callback validates embedded state dispatch", patcher)


if __name__ == "__main__":
    unittest.main()