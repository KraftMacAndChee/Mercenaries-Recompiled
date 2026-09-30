"""Retail D3D state-compiler switch must retain its parent stack frame."""
from __future__ import annotations

import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from tools.recomp import config
from tools.recomp.translator import FunctionTranslator


ROOT = Path(__file__).resolve().parents[2]
XBE = ROOT / "game_files" / "mercenaries-retail" / "default.xbe"
RECOMPILE = ROOT / "ports" / "mercenaries" / "scripts" / "Recompile.ps1"
SEEDS = ROOT / "ports" / "mercenaries" / "manual-seeds.json"
START = 0x002965F0
END = 0x00296B47
INTERIOR_FRAGMENT_SEEDS = {
    0x00296670, 0x002966A0, 0x00296735, 0x002968BE,
    0x002969BE, 0x002969C0, 0x002969C7, 0x002969C9,
    0x002969CB, 0x002969D6, 0x00296A75,
}


def retail_bytes(address: int, size: int) -> bytes:
    config.configure_from_xbe(str(XBE))
    offset = config.va_to_file_offset(address)
    assert offset is not None
    return XBE.read_bytes()[offset:offset + size]


def test_verified_retail_frame_and_shared_epilogue() -> None:
    assert retail_bytes(0x002965FA, 6) == b"\x81\xEC\xB8\x00\x00\x00"
    assert retail_bytes(0x00296668, 2) == b"\xEB\x06"
    assert retail_bytes(0x002966A7, 7) == b"\xFF\x24\x8D\x48\x6B\x29\x00"
    assert retail_bytes(0x00296B3A, 13) == (
        b"\x5F\x5E\x5D\x5B\x81\xC4\xB8\x00\x00\x00\xC2\x08\x00"
    )


def test_retail_switch_lowers_to_local_case_labels() -> None:
    config.configure_from_xbe(str(XBE))
    functions = {
        START: {
            "name": f"sub_{START:08X}",
            "start": f"0x{START:08X}",
            "end": END,
            "size": END - START,
            "_reviewed_range": True,
        },
    }
    source = FunctionTranslator(XBE.read_bytes(), functions).translate_function(
        START, functions[START]
    )

    # The table has 26 retail cases; two following 0x00040201 constants look
    # like code pointers but are not part of this switch.
    assert "switch: 26 entries, 22 targets" in source
    for target in (0x002966AE, 0x002966F8, 0x00296710, 0x00296739,
                   0x002968B9, 0x00296993):
        assert f"goto loc_{target:08X};" in source
        assert f"loc_{target:08X}:" in source
    assert "RECOMP_ITAIL(MEM32(ecx * 4 + 0x296B48))" not in source


def test_canonical_pipeline_coalesces_parent_range() -> None:
    script = RECOMPILE.read_text(encoding="utf-8")
    assert f'"--function-range", "0x{START:08X}:0x{END:08X}"' in script

    seeds = {
        int(entry["start"], 16)
        for entry in json.loads(SEEDS.read_text(encoding="utf-8"))
    }
    assert START in seeds
    assert not (seeds & INTERIOR_FRAGMENT_SEEDS)


if __name__ == "__main__":
    test_verified_retail_frame_and_shared_epilogue()
    test_retail_switch_lowers_to_local_case_labels()
    test_canonical_pipeline_coalesces_parent_range()
    print("ok  d3d_state_compiler_range")
