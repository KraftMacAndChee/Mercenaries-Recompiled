"""Regression coverage for external jump tables targeting function interiors."""

import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', '..'))

from tools.recomp import config
from tools.recomp.translator import (
    FunctionTranslator,
    discover_jump_table_entry_splits,
)


BASE = 0x00014000
TABLE = BASE + 0x100


def test_jump_table_interior_target_becomes_exact_entry_point():
    image = bytearray(0x110)
    # jmp dword ptr [ecx*4 + TABLE]
    image[0:7] = b"\xff\x24\x8d" + struct.pack("<I", TABLE)
    # Parent region: mov [esp+4], eax | ret
    image[7:12] = bytes.fromhex("89442404c3")
    struct.pack_into("<III", image, 0x100, BASE + 11, BASE + 7, 0)

    config._install(
        [config.Section(".text", BASE, len(image), 0, len(image), True)],
        entry_point=BASE,
        kernel_thunk_addr=0,
        origin="jump-table-entry-split-test",
    )
    functions = {
        BASE: {
            "name": f"sub_{BASE:08X}",
            "start": f"0x{BASE:08X}",
            "end": BASE + 7,
            "size": 7,
        },
        BASE + 7: {
            "name": f"sub_{BASE + 7:08X}",
            "start": f"0x{BASE + 7:08X}",
            "end": BASE + 12,
            "size": 5,
        },
    }

    added = discover_jump_table_entry_splits(bytes(image), functions)

    assert added == [BASE + 11]
    assert functions[BASE + 7]["end"] == BASE + 11
    assert functions[BASE + 11]["end"] == BASE + 12
    source = FunctionTranslator(bytes(image), functions).translate_function(
        BASE + 7, functions[BASE + 7]
    )
    assert f"sub_{BASE + 11:08X}(); return; /* fallthrough" in source


def test_local_switch_ignores_trailing_code_like_constants():
    image = bytearray(0x1100)
    # jmp dword ptr [ecx*4 + TABLE]
    image[0:7] = b"\xff\x24\x8d" + struct.pack("<I", TABLE)
    # Two local case entries, both valid instruction boundaries.
    image[7:12] = bytes.fromhex("89442404c3")
    # The third dword is valid code in the section but outside this function;
    # it models adjacent data that only resembles another jump-table entry.
    struct.pack_into("<IIII", image, 0x100,
                     BASE + 11, BASE + 7, BASE + 0x1000, 0)

    config._install(
        [config.Section(".text", BASE, len(image), 0, len(image), True)],
        entry_point=BASE,
        kernel_thunk_addr=0,
        origin="jump-table-local-prefix-test",
    )
    functions = {
        BASE: {
            "name": f"sub_{BASE:08X}",
            "start": f"0x{BASE:08X}",
            "end": BASE + 12,
            "size": 12,
        },
    }

    source = FunctionTranslator(bytes(image), functions).translate_function(
        BASE, functions[BASE]
    )

    assert "switch: 2 entries, 2 targets" in source
    assert f"goto loc_{BASE + 7:08X};" in source
    assert f"goto loc_{BASE + 11:08X};" in source
    assert f"loc_{BASE + 7:08X}:" in source
    assert f"loc_{BASE + 11:08X}:" in source


def test_local_switch_accepts_folded_cases_with_one_shared_epilogue():
    image = bytearray(0x120)
    # All four native switch cases share the same local return epilogue.  This
    # is the shape used by retail Mercenaries' RsActorItemBounty::CanAutoUse.
    image[0:7] = b"\xff\x24\x8d" + struct.pack("<I", TABLE)
    image[7:12] = bytes.fromhex("5fb0015ec3")
    struct.pack_into("<IIIII", image, 0x100,
                     BASE + 7, BASE + 7, BASE + 7, BASE + 7, 0)

    config._install(
        [config.Section(".text", BASE, len(image), 0, len(image), True)],
        entry_point=BASE,
        kernel_thunk_addr=0,
        origin="jump-table-folded-local-cases-test",
    )
    functions = {
        BASE: {
            "name": f"sub_{BASE:08X}",
            "start": f"0x{BASE:08X}",
            "end": BASE + 12,
            "size": 12,
        },
    }

    source = FunctionTranslator(bytes(image), functions).translate_function(
        BASE, functions[BASE]
    )

    assert "switch: 4 entries, 1 targets" in source
    assert f"goto loc_{BASE + 7:08X};" in source
    assert f"loc_{BASE + 7:08X}:" in source
    assert "RECOMP_ITAIL" in source  # fallback remains for malformed indices


def test_jump_table_materializes_verified_case_gap():
    image = bytearray(0x110)
    # Dispatcher ends at +7. Its optimized case bodies occupy the executable
    # gap up to the next detected function at +12.
    image[0:7] = b"\xff\x24\x8d" + struct.pack("<I", TABLE)
    image[7:12] = bytes.fromhex("31f6eb0190")
    image[12] = 0xC3
    struct.pack_into("<III", image, 0x100, BASE + 7, BASE + 11, 0)

    config._install(
        [config.Section(".text", BASE, len(image), 0, len(image), True)],
        entry_point=BASE,
        kernel_thunk_addr=0,
        origin="jump-table-gap-entry-test",
    )
    functions = {
        BASE: {
            "name": f"sub_{BASE:08X}",
            "start": f"0x{BASE:08X}",
            "end": BASE + 7,
            "size": 7,
        },
        BASE + 12: {
            "name": f"sub_{BASE + 12:08X}",
            "start": f"0x{BASE + 12:08X}",
            "end": BASE + 13,
            "size": 1,
        },
    }

    added = discover_jump_table_entry_splits(bytes(image), functions)

    assert added == [BASE + 7, BASE + 11]
    assert functions[BASE + 7]["end"] == BASE + 11
    assert functions[BASE + 11]["end"] == BASE + 12


def test_jump_table_materializes_single_case_at_gap_start():
    image = bytearray(0x110)
    # The table is credible because it has two consecutive code targets, but
    # only its first case occupies the executable gap. The second target is
    # the already-detected function immediately after that gap.
    image[0:7] = b"\xff\x24\x8d" + struct.pack("<I", TABLE)
    image[7:12] = bytes.fromhex("8b442404c3")
    image[12] = 0xC3
    struct.pack_into("<III", image, 0x100, BASE + 7, BASE + 12, 0)

    config._install(
        [config.Section(".text", BASE, len(image), 0, len(image), True)],
        entry_point=BASE,
        kernel_thunk_addr=0,
        origin="jump-table-single-gap-entry-test",
    )
    functions = {
        BASE: {
            "name": f"sub_{BASE:08X}",
            "start": f"0x{BASE:08X}",
            "end": BASE + 7,
            "size": 7,
        },
        BASE + 12: {
            "name": f"sub_{BASE + 12:08X}",
            "start": f"0x{BASE + 12:08X}",
            "end": BASE + 13,
            "size": 1,
        },
    }

    added = discover_jump_table_entry_splits(bytes(image), functions)

    assert added == [BASE + 7]
    assert functions[BASE + 7]["end"] == BASE + 12


if __name__ == "__main__":
    test_jump_table_interior_target_becomes_exact_entry_point()
    test_local_switch_ignores_trailing_code_like_constants()
    test_local_switch_accepts_folded_cases_with_one_shared_epilogue()
    test_jump_table_materializes_verified_case_gap()
    test_jump_table_materializes_single_case_at_gap_start()
    print("ok  jump_table_entry_splits")
