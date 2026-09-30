"""Keep the retail encounter reader reachable through its two-entry vtable."""

import json
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
PORT = ROOT / "ports" / "mercenaries"
GENERATED = PORT / "src" / "recomp" / "gen"
READER = 0x00122910
READER_END = 0x00122BA1
VTABLE = 0x002F0C58


class EncounterReaderVtableTests(unittest.TestCase):
    def test_two_entry_vtable_reader_is_classified_and_generated(self) -> None:
        identified = json.loads(
            (PORT / "analysis" / "func_id" / "identified_functions.json")
            .read_text(encoding="utf-8")
        )
        reader = next(
            item for item in identified if int(item["start"], 0) == READER
        )
        self.assertEqual(int(reader["end"], 0), READER_END)
        self.assertEqual(reader["category"], "game_vtable")
        self.assertEqual(int(reader["vtable_addr"], 0), VTABLE)
        self.assertEqual(reader["vtable_index"], 1)

        symbol = "sub_00122910"
        header = (GENERATED / "recomp_funcs.h").read_text(encoding="utf-8")
        dispatch = (GENERATED / "recomp_dispatch.c").read_text(encoding="utf-8")
        body = "\n".join(
            path.read_text(encoding="utf-8")
            for path in sorted(GENERATED.glob("recomp_[0-9][0-9][0-9][0-9].c"))
            if symbol in path.read_text(encoding="utf-8")
        )
        self.assertIn(f"void {symbol}(void);", header)
        self.assertIn(f"(recomp_func_t){symbol}", dispatch)
        self.assertIn("MEM32(ebx + 4) = eax;", body)
        self.assertIn("Original: 0x00122910 - 0x00122BA1", body)


if __name__ == "__main__":
    unittest.main()
