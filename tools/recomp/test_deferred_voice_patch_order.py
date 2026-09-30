"""Verify deferred callback patches work on fresh and already wrapped output."""
from pathlib import Path
import runpy
import unittest

ROOT = Path(__file__).resolve().parents[2]


class DeferredVoicePatchOrder(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.patcher = runpy.run_path(str(ROOT / "ports/mercenaries/scripts/Patch-Generated.py"))

    def test_fresh_output_and_idempotence(self):
        text = "void sub_0011EA30(void)\n{\n"
        for site, owner in (("0011EB50", "edi"), ("0011EB5E", "eax")):
            text += (f"loc_{site}: ;\n    PUSH32(esp, ebx);\n    ecx = {owner};\n"
                     "    PUSH32(esp, 0); sub_00113A50(); /* call 0x00113A50 */\n\n")
        text += "}\n"
        wrap = self.patcher["trace_event_abi_calls"]
        patch = self.patcher["patch_deferred_voice_calls"]
        result = patch(wrap(text))
        self.assertEqual(result.count("PUSH32(esp, 0); recomp_voice_fallback_call();"), 2)
        self.assertEqual(result.count("recomp_event_abi_checkpoint(0x0011EA30u"), 2)
        self.assertEqual(patch(wrap(result)), result)
        self.assertEqual(patch("void unrelated(void) {}"), "void unrelated(void) {}")
        with self.assertRaises(RuntimeError):
            patch(text)  # A missing prerequisite must fail, not silently skip.


if __name__ == "__main__":
    unittest.main()
