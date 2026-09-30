"""Regression coverage for x86 rotate-through-carry instructions."""

import os
import sys
from pathlib import Path

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.recomp import config  # noqa: E402
from tools.recomp.translator import FunctionTranslator  # noqa: E402

BASE = 0x00018000
ROOT = Path(__file__).resolve().parents[2]
RETAIL_GEN = (
    ROOT / "ports" / "mercenaries" / "src" / "recomp" / "gen" / "recomp_0011.c"
).read_text(encoding="utf-8")


def _translate(code: bytes) -> str:
    config._install(
        [config.Section(".text", BASE, len(code), 0, len(code), True)],
        entry_point=BASE, kernel_thunk_addr=0, origin="rotate-carry-test",
    )
    info = {
        "name": f"sub_{BASE:08X}", "start": f"0x{BASE:08X}",
        "end": BASE + len(code), "size": len(code),
    }
    return FunctionTranslator(code, {BASE: info}).translate_function(BASE, info)


def test_rcr_and_rcl_preserve_the_carry_chain():
    source = _translate(bytes.fromhex(
        "d1eb"  # shr ebx, 1 (provides carry to RCR)
        "d1d9"  # rcr ecx, 1
        "d3d8"  # rcr eax, cl
        "d1d0"  # rcl eax, 1
        "c3"
    ))
    assert "TODO: rcr" not in source
    assert "TODO: rcl" not in source
    assert "int _cf = 0;" in source
    assert "uint32_t _shift_next_cf = _shift_value & 1u;" in source
    assert "_cf = (int)_shift_next_cf;" in source
    assert source.count("_rot_next_cf") == 6
    assert "_rot_value & 1u" in source
    assert "((uint32_t)_cf & 1u) << 31u" in source
    assert "(_rot_value >> 31u) & 1u" in source
    assert "((_rot_value << 1u) & 0xFFFFFFFFu)" in source


def test_retail_division_helpers_have_no_missing_rotate_carry():
    assert "TODO: rcr" not in RETAIL_GEN
    assert "TODO: rcl" not in RETAIL_GEN
    assert RETAIL_GEN.count("/* rcr */") == 10
    assert "((uint32_t)_cf & 1u) << 31u" in RETAIL_GEN


if __name__ == "__main__":
    test_rcr_and_rcl_preserve_the_carry_chain()
    test_retail_division_helpers_have_no_missing_rotate_carry()
    print("ok rotate_through_carry")

