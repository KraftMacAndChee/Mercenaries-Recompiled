"""Keep the South Korean contract's indirect Lua closure in every ISO rebuild."""

import json
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SEEDS = ROOT / "ports" / "mercenaries" / "manual-seeds.json"
GENERATED = ROOT / "ports" / "mercenaries" / "src" / "recomp" / "gen"
CALLBACK = 0x00120ED0


class SouthKoreanContractLuaCallbackSeedTests(unittest.TestCase):
    def test_indirect_callback_is_reviewed_and_generated(self) -> None:
        seeds = json.loads(SEEDS.read_text(encoding="utf-8"))
        self.assertTrue(
            any(int(item["start"], 0) == CALLBACK for item in seeds),
            "the retail Lua closure must bypass the interior-address classifier",
        )

        header = (GENERATED / "recomp_funcs.h").read_text(encoding="utf-8")
        dispatch = (GENERATED / "recomp_dispatch.c").read_text(encoding="utf-8")
        symbol = "sub_00120ED0"
        self.assertIn(f"void {symbol}(void);", header)
        self.assertIn(f"(recomp_func_t){symbol}", dispatch)


if __name__ == "__main__":
    unittest.main()
