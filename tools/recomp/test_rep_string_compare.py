"""Regression coverage for REP CMPS/SCAS equality flag lifting."""

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.recomp import config  # noqa: E402
from tools.recomp.translator import FunctionTranslator  # noqa: E402


BASE = 0x00012000


def _translate(code: bytes) -> str:
    config._install(
        [config.Section(".text", BASE, len(code), 0, len(code), True)],
        entry_point=BASE,
        kernel_thunk_addr=0,
        origin="rep-string-compare-test",
    )
    functions = {
        BASE: {
            "name": f"sub_{BASE:08X}",
            "start": f"0x{BASE:08X}",
            "end": BASE + len(code),
            "size": len(code),
        }
    }
    return FunctionTranslator(code, functions).translate_function(
        BASE, functions[BASE]
    )


def test_repe_cmpsb_updates_zf_condition():
    source = _translate(bytes.fromhex("f3a67501c3c3"))

    assert "uint32_t _cmps_lhs = MEM8(esi);" in source
    assert "_flags = (_cmps_lhs == _cmps_rhs);" in source
    assert "if (_flags == 0) break;" in source
    assert "(_flags == 0) /* REP string comparison !ZF */" in source
    assert "strings differed" not in source


def test_repne_scasb_stops_on_match():
    source = _translate(bytes.fromhex("f2ae7401c3c3"))

    assert "uint32_t _scas_lhs = LO8(eax);" in source
    assert "_flags = (_scas_lhs == _scas_rhs);" in source
    assert "if (_flags != 0) break;" in source
    assert "(_flags != 0) /* REP string comparison ZF */" in source


def test_std_repne_scasb_scans_backwards():
    source = _translate(bytes.fromhex("fdf2aefcc3"))

    assert "edi -= 1u; --ecx;" in source


def test_std_rep_movsd_copies_backwards():
    source = _translate(bytes.fromhex("fdf3a5fcc3"))

    assert "MEM32(edi - _i*4) = MEM32(esi - _i*4);" in source
    assert "esi -= ecx * 4; edi -= ecx * 4;" in source


def test_std_rep_stosb_writes_backwards():
    source = _translate(bytes.fromhex("fdf3aafcc3"))

    assert "MEM8(edi - _i) = LO8(eax);" in source
    assert "edi -= ecx; ecx = 0;" in source

if __name__ == "__main__":
    test_repe_cmpsb_updates_zf_condition()
    test_repne_scasb_stops_on_match()
    test_std_repne_scasb_scans_backwards()
    test_std_rep_movsd_copies_backwards()
    test_std_rep_stosb_writes_backwards()
    print("ok  rep_string_compare")