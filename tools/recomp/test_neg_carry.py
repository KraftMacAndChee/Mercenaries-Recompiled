"""
Regression test for NEG carry propagation.

Run: py -3 tools/recomp/test_neg_carry.py

The Xbox CRT/XACT code commonly uses neg reg; sbb reg, reg to convert a
pointer into 0 or -1. NEG must set CF when its original operand is nonzero.
"""

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.recomp import config  # noqa: E402
from tools.recomp.translator import FunctionTranslator  # noqa: E402


BASE = 0x00012000
CODE = bytes.fromhex(
    "f7de"  # neg esi
    "1bf6"  # sbb esi, esi
    "c3"    # ret
)


def test_neg_sets_carry_before_sbb():
    config._install(
        [config.Section(".text", BASE, len(CODE), 0, len(CODE), True)],
        entry_point=BASE,
        kernel_thunk_addr=0,
        origin="neg-carry-test",
    )
    functions = {
        BASE: {
            "name": f"sub_{BASE:08X}",
            "start": f"0x{BASE:08X}",
            "end": BASE + len(CODE),
            "size": len(CODE),
        }
    }
    source = FunctionTranslator(CODE, functions).translate_function(
        BASE, functions[BASE]
    )

    assert "int _cf = 0; /* carry flag */" in source
    assert "_cf = (_neg_value != 0);" in source
    assert "_cf ? 0xFFFFFFFF : 0" in source


def test_add_adc_chain_propagates_each_carry():
    code = bytes.fromhex(
        "81c1ffffff7f"  # add ecx, 0x7fffffff
        "83d000"        # adc eax, 0
        "83d200"        # adc edx, 0
        "c3"
    )
    config._install(
        [config.Section(".text", BASE, len(code), 0, len(code), True)],
        entry_point=BASE,
        kernel_thunk_addr=0,
        origin="add-adc-carry-test",
    )
    functions = {
        BASE: {
            "name": f"sub_{BASE:08X}",
            "start": f"0x{BASE:08X}",
            "end": BASE + len(code),
            "size": len(code),
        }
    }
    source = FunctionTranslator(code, functions).translate_function(
        BASE, functions[BASE]
    )
    assert "_cf = ((uint64_t)_alu_dst + (uint64_t)_alu_src > 0xFFFFFFFFull);" in source
    assert source.count("uint64_t _adc_wide") == 2
    assert source.count("_cf = (_adc_wide > 0xFFFFFFFFull);") == 2

if __name__ == "__main__":
    test_neg_sets_carry_before_sbb()
    test_add_adc_chain_propagates_each_carry()
    print("ok  neg_sets_carry_before_sbb")
