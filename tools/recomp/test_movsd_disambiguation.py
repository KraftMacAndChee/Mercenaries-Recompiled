"""Regression tests for the x86 string/SSE2 MOVSD mnemonic collision."""

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.recomp import config  # noqa: E402
from tools.recomp.translator import FunctionTranslator  # noqa: E402


BASE = 0x00015000


def _translate(code: bytes) -> str:
    config._install(
        [config.Section(".text", BASE, len(code), 0, len(code), True)],
        entry_point=BASE,
        kernel_thunk_addr=0,
        origin="movsd-disambiguation-test",
    )
    fn = {
        "name": f"sub_{BASE:08X}",
        "start": f"0x{BASE:08X}",
        "end": BASE + len(code),
        "size": len(code),
    }
    return FunctionTranslator(code, {BASE: fn}).translate_function(BASE, fn)


def test_string_movsd_copies_one_dword_and_advances_indices():
    source = _translate(bytes.fromhex("a5c3"))
    assert "MEM32(edi) = MEM32(esi);" in source
    assert "esi += 4; edi += 4;" in source
    assert "unsupported operands" not in source


def test_sse2_movsd_remains_an_xmm_operation():
    source = _translate(bytes.fromhex("f20f10c1c3"))
    assert "recomp_xmm_copysd(xmm0v, xmm1v);" in source
    assert "MEM32(edi) = MEM32(esi);" not in source


if __name__ == "__main__":
    test_string_movsd_copies_one_dword_and_advances_indices()
    test_sse2_movsd_remains_an_xmm_operation()
    print("ok  movsd_disambiguation")