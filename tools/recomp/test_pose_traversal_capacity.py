import importlib.util
import sys
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
PATCHER_PATH = ROOT / "ports/mercenaries/scripts/Patch-Generated.py"
GENERATED = ROOT / "ports/mercenaries/src/recomp/gen/recomp_0010.c"
TYPES = ROOT / "ports/mercenaries/src/recomp/recomp_types.h"
MANUAL = ROOT / "ports/mercenaries/src/recomp_manual.c"


def load_patcher():
    spec = importlib.util.spec_from_file_location("merc_patch_generated", PATCHER_PATH)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


class PoseTraversalCapacityTests(unittest.TestCase):
    def test_guard_is_generated_and_idempotent(self):
        module = load_patcher()
        generated = GENERATED.read_text(encoding="utf-8")
        patched = module.patch_pose_traversal_capacity(generated)

        self.assertEqual(
            patched.count("/* RedPose traversal capacity: child */"), 1
        )
        self.assertEqual(
            patched.count("/* RedPose traversal capacity: sibling */"), 1
        )
        self.assertEqual(patched.count("if (ebx >= 256u)"), 2)
        self.assertEqual(
            patched.count("/* RedPose traversal validity: loop head */"), 1
        )
        self.assertIn("uint32_t _recomp_pose_visits = 0u;", patched)
        self.assertIn("ebx == 0u || ebx > 256u", patched)
        self.assertIn("++_recomp_pose_visits > 256u", patched)
        self.assertGreaterEqual(patched.count("goto loc_00204632;"), 3)
        self.assertEqual(patched.count("recomp_pose_traversal_overflow("), 3)
        self.assertEqual(module.patch_pose_traversal_capacity(patched), patched)

    def test_diagnostic_is_declared_and_implemented(self):
        types = TYPES.read_text(encoding="utf-8")
        manual = MANUAL.read_text(encoding="utf-8")
        self.assertIn("void recomp_pose_traversal_overflow(", types)
        self.assertIn("void recomp_pose_traversal_overflow(", manual)
        self.assertIn("[REDPOSE-STACK]", manual)
        self.assertIn("current=%08X", manual)
        self.assertIn("[REDPOSE-STACK] recent[%u]", manual)
        self.assertIn("g_recomp_recent_game_funcs[index]", manual)


if __name__ == "__main__":
    unittest.main()
