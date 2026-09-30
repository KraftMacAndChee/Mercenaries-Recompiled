from pathlib import Path
import unittest

from generated_test_utils import generated_text_containing

ROOT = Path(__file__).resolve().parents[2]


class ForcedWalkTraceTests(unittest.TestCase):
    def test_full_stick_anomaly_trace_is_diagnostic_only(self):
        manual = (ROOT / "ports/mercenaries/src/recomp_manual.c").read_text()
        generated_jump = generated_text_containing("recomp_human_jump_checkpoint(0u, esi, eax, 0u);")
        generated_move = generated_text_containing("recomp_human_move_selection_checkpoint(esi, xmm1);")
        patcher = (ROOT / "ports/mercenaries/scripts/Patch-Generated.py").read_text()
        self.assertIn('getenv("MERCENARIES_TRACE_FORCED_WALK")', manual)
        self.assertIn("stick_magnitude >= 0.95f", manual)
        self.assertIn("[HUMAN-MOVE-STATE]", manual)
        self.assertIn("recomp_human_move_selection_checkpoint(esi, xmm1);", generated_move)
        self.assertIn("retail human move selection forced-walk diagnostic", patcher)
        self.assertIn("retail human jump request diagnostic", patcher)
        self.assertIn("recomp_human_jump_checkpoint(0u, esi, eax, 0u);", generated_jump)
        self.assertIn(
            "if (valid && guest_u32(actor) == 0x002E32B8u)", manual
        )
        self.assertNotIn(
            "if (valid)\n        g_recomp_live_human_actor = actor;", manual
        )
        self.assertNotIn("MEM8(physics + 0x65u) =", manual)

    def test_no_title_specific_landing_run_override(self):
        manual = (ROOT / "ports/mercenaries/src/recomp_manual.c").read_text()
        patcher = (ROOT / "ports/mercenaries/scripts/Patch-Generated.py").read_text()
        generated_player = generated_text_containing("void sub_0005D5C0(void)")
        generated_move = generated_text_containing("loc_0013E007: ;")
        for source in (manual, patcher, generated_player, generated_move):
            self.assertNotIn("recomp_human_landing_control", source)
            self.assertNotIn("recomp_human_apply_landing_run_hysteresis", source)
            self.assertNotIn("landing_control_value", source)
        self.assertNotIn("0.90f", manual)

if __name__ == "__main__":
    unittest.main()