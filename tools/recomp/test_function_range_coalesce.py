"""Regression coverage for reviewed native control-flow range coalescing."""

import json
import os
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from unittest.mock import patch

from tools.recomp.translator import BatchTranslator


def test_function_range_removes_interior_entries():
    with tempfile.TemporaryDirectory() as temporary:
        xbe_path = os.path.join(temporary, "default.xbe")
        functions_path = os.path.join(temporary, "functions.json")
        with open(xbe_path, "wb") as stream:
            stream.write(b"\0" * 32)
        with open(functions_path, "w", encoding="utf-8") as stream:
            json.dump([
                {"start": "0x00011000", "end": "0x00011010", "size": 16},
                {"start": "0x00011010", "end": "0x00011020", "size": 16},
                {"start": "0x00011020", "end": "0x00011030", "size": 16},
            ], stream)

        translator = BatchTranslator(
            xbe_path, functions_path, discover_entry_splits=False,
            function_ranges=[(0x00011000, 0x00011030)],
            seh_prolog=0, seh_epilog=0)

        assert sorted(translator.func_db) == [0x00011000]
        assert translator.func_db[0x00011000]["end"] == 0x00011030
        assert translator.func_db[0x00011000]["size"] == 0x30
        assert translator.function_ranges == [(0x00011000, 0x00011030)]


def test_function_range_survives_global_entry_split_discovery():
    with tempfile.TemporaryDirectory() as temporary:
        xbe_path = os.path.join(temporary, "default.xbe")
        functions_path = os.path.join(temporary, "functions.json")
        with open(xbe_path, "wb") as stream:
            stream.write(b"\0" * 32)
        with open(functions_path, "w", encoding="utf-8") as stream:
            json.dump([
                {"start": "0x00011000", "end": "0x00011030", "size": 48},
            ], stream)

        def rediscover_interior(_xbe_data, func_db):
            owner = func_db[0x00011000]
            owner["end"] = 0x00011010
            owner["size"] = 0x10
            func_db[0x00011010] = {
                "start": "0x00011010", "end": 0x00011030,
                "size": 0x20, "_addr": 0x00011010,
                "name": "sub_00011010",
            }
            return [0x00011010]

        with patch("tools.recomp.translator.discover_jump_table_entry_splits",
                   side_effect=rediscover_interior):
            translator = BatchTranslator(
                xbe_path, functions_path, discover_entry_splits=True,
                function_ranges=[(0x00011000, 0x00011030)],
                seh_prolog=0, seh_epilog=0)

        assert sorted(translator.func_db) == [0x00011000]
        assert translator.func_db[0x00011000]["end"] == 0x00011030
        assert translator.func_db[0x00011000]["size"] == 0x30
        assert translator.jump_table_entry_splits == []


if __name__ == "__main__":
    test_function_range_removes_interior_entries()
    test_function_range_survives_global_entry_split_discovery()
    print("ok  function_range_coalesce")