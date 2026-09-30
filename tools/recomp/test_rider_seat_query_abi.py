"""Verify the retail rider seat query honors the x86 callee-save ABI."""

from pathlib import Path
import runpy
import unittest

ROOT = Path(__file__).resolve().parents[2]


class RiderSeatQueryAbiTests(unittest.TestCase):
    def test_query_restores_every_nonvolatile_on_both_returns(self) -> None:
        patcher = runpy.run_path(
            str(ROOT / "ports/mercenaries/scripts/Patch-Generated.py")
        )
        patches = {item.name: item for item in patcher["PATCHES"]}
        generated = (
            ROOT / "ports/mercenaries/src/recomp/gen/recomp_0006.c"
        ).read_text(encoding="utf-8")
        names = (
            "retail rider seat query snapshots nonvolatile registers",
            "retail rider seat query restores nonvolatile registers on success",
            "retail rider seat query restores nonvolatile registers on failure",
        )
        for name in names:
            self.assertIn(patches[name].after, generated)
        function = generated[
            generated.index("void sub_001624B0(void)") : generated.index(
                "void sub_001624F0(void)"
            )
        ]
        for register in ("ebx", "esi", "edi"):
            self.assertIn(
                f"const uint32_t rider_query_saved_{register} = {register};",
                function,
            )
            self.assertEqual(
                function.count(f"{register} = rider_query_saved_{register};"),
                2,
            )
        self.assertEqual(
            function.count("g_seh_ebp = rider_query_saved_ebp;"), 2
        )


if __name__ == "__main__":
    unittest.main()