from pathlib import Path
import runpy
import unittest

from generated_test_utils import generated_text_containing

ROOT = Path(__file__).resolve().parents[2]


class DataPodFlashTraceTests(unittest.TestCase):
    def test_trace_measures_authored_timer_without_retuning_flash(self):
        manual = (ROOT / "ports/mercenaries/src/recomp_manual.c").read_text(
            encoding="utf-8-sig"
        )
        generated = generated_text_containing(
            "recomp_datapod_timer_checkpoint(esi, xmm0, xmm1);"
        )
        shared_tail = generated_text_containing("loc_000CB470: ;")
        patcher = (
            ROOT / "ports/mercenaries/scripts/Patch-Generated.py"
        ).read_text(encoding="utf-8-sig")
        runner = (
            ROOT / "tools/recomp/Run-HiddenRetailManual.ps1"
        ).read_text(encoding="utf-8-sig")
        self.assertIn("[DATAPOD-FLASH-TIMER]", manual)
        self.assertIn("recomp_datapod_timer_checkpoint(esi, xmm0, xmm1);", generated)
        self.assertIn("retail DataPod flash timer diagnostic", patcher)
        self.assertIn("retail DataPod Status shared-tail interior entries", patcher)
        patch_state = runpy.run_path(
            str(ROOT / "ports/mercenaries/scripts/Patch-Generated.py")
        )
        inline = patch_state["TRANSLATOR_INLINED_PATCHES"][
            "retail DataPod Status shared-tail interior entries"
        ]
        self.assertIn(inline, shared_tail)
        self.assertIn("MERCENARIES_TRACE_DATAPOD_FLASH", runner)
        self.assertNotIn("DATAPOD_FLASH_RATE_OVERRIDE", manual)
        self.assertNotIn("DATAPOD_FLASH_RATE_OVERRIDE", generated)


if __name__ == "__main__":
    unittest.main()