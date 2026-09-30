"""Regression coverage for retail front-end callback discovery.

The callback table rooted at 0x002ED308 contains a small number of valid
straight-line callbacks. Losing the table caused Lua to skip the final menu
callbacks and diverge immediately after front-end construction.
"""

from __future__ import annotations

import json
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SEEDS = ROOT / "ports/mercenaries/analysis/seeds.json"
GENERATED = ROOT / "ports/mercenaries/src/recomp/gen"
FRONTEND_CALLBACKS = (0x00114DB0, 0x00114E00)


class MercenariesFrontendCallbackSeedTests(unittest.TestCase):
    def test_callbacks_are_seeded_and_emitted_for_indirect_dispatch(self) -> None:
        seeds = json.loads(SEEDS.read_text(encoding="utf-8"))
        starts = {
            int(item["start"], 0)
            for item in seeds
            if isinstance(item, dict) and "start" in item
        }

        header = (GENERATED / "recomp_funcs.h").read_text(encoding="utf-8")
        dispatch = (GENERATED / "recomp_dispatch.c").read_text(encoding="utf-8")
        bodies = "\n".join(
            path.read_text(encoding="utf-8")
            for path in sorted(GENERATED.glob("recomp_[0-9][0-9][0-9][0-9].c"))
        )

        for address in FRONTEND_CALLBACKS:
            symbol = f"sub_{address:08X}"
            self.assertIn(address, starts, f"retail callback {symbol} is not seeded")
            self.assertRegex(header, rf"\bvoid\s+{symbol}\s*\(void\);")
            self.assertRegex(bodies, rf"\bvoid\s+{symbol}\s*\(void\)\s*\n\{{")
            self.assertRegex(
                dispatch,
                rf"\{{\s*0x{address:08X}u,\s*\(recomp_func_t\){symbol}\s*\}}",
            )


if __name__ == "__main__":
    unittest.main()