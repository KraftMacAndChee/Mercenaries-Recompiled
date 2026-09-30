"""Bounds and bit-refill regressions for the read-only retail script decoder."""
import importlib.util
from pathlib import Path
import struct
import unittest

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "retail_script", ROOT / "tools/diagnostics/inspect_retail_script.py")
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class RetailScriptTests(unittest.TestCase):
    def test_literal(self):
        self.assertEqual(MODULE.decompress(b"\x05\x00A\x00\x00\x00", 1), b"A")

    def test_small_back_reference(self):
        self.assertEqual(MODULE.decompress(b"\x07\x01ABC\xfd\x00\x00\x00", 6), b"ABCABC")

    def test_overlapping_big_back_reference(self):
        self.assertEqual(MODULE.decompress(b"\x15\x00A\xff\xf1\x00\x00\x00", 5), b"AAAAA")

    def test_refill_precedes_sixteenth_literal(self):
        text = b"abcdefghijklmnop"
        encoded = b"\xff\xff" + text[:15] + b"\x02\x00" + text[15:] + b"\0\0\0"
        self.assertEqual(MODULE.decompress(encoded, 16), text)

    def test_invalid_streams(self):
        for data, expected in ((b"", 1), (b"\0", 1),
                               (b"\0\0\xff", 3),
                               (b"\x05\0A\0\0\0", 2),
                               (b"\x15\0A\xff\xf1\0\0\0", 4)):
            with self.subTest(data=data), self.assertRaises(ValueError):
                MODULE.decompress(data, expected)

    def test_chunk_bounds_and_padding(self):
        data = struct.pack("<4sI", b"NAME", 1) + b"A\0\0\0"
        self.assertEqual(list(MODULE.chunks(data)), [(b"NAME", b"A")])
        with self.assertRaises(ValueError):
            list(MODULE.chunks(data[:8]))

    def test_local_retail_reference(self):
        archive = ROOT / "game_files/mercenaries-retail/DATAxbox/assets.dsk"
        if not archive.exists():
            self.skipTest("User-owned retail data not installed")
        script = MODULE.read_script(archive, "briefing_utilities")
        self.assertIn("Actor_EnableScriptedUse('starter_trigger', TRUE", script)
        self.assertIn("Utility_WriteNumberToScribbleMemory('mission_accepted', 1)", script)


if __name__ == "__main__":
    unittest.main(verbosity=2)
