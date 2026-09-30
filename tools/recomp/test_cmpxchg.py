"""Regression tests for CMPXCHG semantics and flag consumption.

Run: py -3 tools/recomp/test_cmpxchg.py
"""

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.recomp import config  # noqa: E402
from tools.recomp.translator import FunctionTranslator  # noqa: E402


BASE = 0x00012000


def translate(code):
    config._install(
        [config.Section(".text", BASE, len(code), 0, len(code), True)],
        entry_point=BASE,
        kernel_thunk_addr=0,
        origin="cmpxchg-test",
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


def test_cmpxchg32_retry_loop():
    source = translate(bytes.fromhex(
        "8b01"      # mov eax, [ecx]
        "f00fb111"  # lock cmpxchg [ecx], edx
        "75fa"      # jne back to cmpxchg
        "c3"
    ))
    assert "uint32_t _cmpxchg_dest = (uint32_t)(MEM32(ecx));" in source
    assert "_flags = (_cmpxchg_dest == _cmpxchg_acc);" in source
    assert "MEM32(ecx) = (uint32_t)_cmpxchg_src;" in source
    assert "else { eax = _cmpxchg_dest; }" in source
    assert "if ((_flags == 0)) goto" in source
    assert "TODO: cmpxchg" not in source


def test_cmpxchg8_updates_al():
    source = translate(bytes.fromhex(
        "f00fb00b"  # lock cmpxchg [ebx], cl
        "c3"
    ))
    assert "uint8_t _cmpxchg_dest" in source
    assert "SET_LO8(eax, _cmpxchg_dest);" in source
    assert "int _flags = 0;" in source


if __name__ == "__main__":
    test_cmpxchg32_retry_loop()
    test_cmpxchg8_updates_al()
    print("ok  cmpxchg_semantics")