"""Verify rider seat state updates preserve the caller's x86 ABI state."""

from pathlib import Path
import runpy
import unittest

from generated_test_utils import generated_text_containing

ROOT = Path(__file__).resolve().parents[2]


class RiderSeatUpdateAbiTests(unittest.TestCase):
    def test_update_restores_all_caller_nonvolatiles(self) -> None:
        patcher = runpy.run_path(
            str(ROOT / "ports/mercenaries/scripts/Patch-Generated.py")
        )
        patches = {item.name: item for item in patcher["PATCHES"]}
        generated = generated_text_containing("void sub_001683D0(void)")
        names = (
            "retail rider seat update snapshots caller nonvolatiles",
            "retail rider seat update restores caller nonvolatiles",
        )
        for name in names:
            self.assertIn(patches[name].after, generated)
        function = generated[
            generated.index("void sub_001683D0(void)") : generated.index(
                "void sub_001688E0(void)"
            )
        ]
        for register in ("ebx", "esi", "edi"):
            self.assertIn(
                f"const uint32_t rider_update_saved_{register} = {register};",
                function,
            )
            self.assertIn(
                f"{register} = rider_update_saved_{register};", function
            )
        self.assertIn("g_seh_ebp = rider_update_saved_ebp;", function)


if __name__ == "__main__":
    unittest.main()