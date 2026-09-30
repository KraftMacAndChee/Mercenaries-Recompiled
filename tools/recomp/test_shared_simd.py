"""Regression test for SSE state crossing translated function calls."""

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.recomp import config  # noqa: E402
from tools.recomp.translator import FunctionTranslator  # noqa: E402


BASE = 0x00014000
# callee: movss xmm0,[0x15000]; ret
# caller: call callee; movss [0x15004],xmm0; ret
CODE = bytes.fromhex("f30f100500500100c3e8f2fffffff30f110504500100c3")
CALLEE_END = BASE + 9


def test_xmm_register_survives_translated_call():
    config._install(
        [config.Section(".text", BASE, len(CODE), 0, len(CODE), True)],
        entry_point=BASE,
        kernel_thunk_addr=0,
        origin="shared-simd-test",
    )
    functions = {
        BASE: {
            "name": f"sub_{BASE:08X}",
            "start": f"0x{BASE:08X}",
            "end": CALLEE_END,
            "size": CALLEE_END - BASE,
        },
        CALLEE_END: {
            "name": f"sub_{CALLEE_END:08X}",
            "start": f"0x{CALLEE_END:08X}",
            "end": BASE + len(CODE),
            "size": BASE + len(CODE) - CALLEE_END,
        },
    }
    translator = FunctionTranslator(CODE, functions)
    callee = translator.translate_function(BASE, functions[BASE])
    caller = translator.translate_function(CALLEE_END, functions[CALLEE_END])

    assert "float xmm0" not in callee
    assert "float xmm0" not in caller
    assert "recomp_xmm_loadss(xmm0v, 0x15000);" in callee
    assert "sub_00014000();" in caller
    assert "MEMF(0x15004) = xmm0;" in caller

    template = os.path.join(
        os.path.dirname(__file__), "..", "..", "templates", "runtime",
        "recomp_types.h",
    )
    with open(template, "r", encoding="utf-8") as stream:
        header = stream.read()
    assert "extern float g_xmm0[4]" in header
    assert "#define xmm0 g_xmm0[0]" in header
    assert "#define xmm0v g_xmm0" in header
    assert "extern uint64_t g_mm0" in header


def test_mmx_movq_preserves_full_64_bit_payload():
    # movq mm0,[0x15000]; movq [0x15008],mm0; movntq [0x15010],mm0; ret
    code = bytes.fromhex("0f6f05005001000f7f05085001000fe70510500100c3")
    config._install(
        [config.Section(".text", BASE, len(code), 0, len(code), True)],
        entry_point=BASE,
        kernel_thunk_addr=0,
        origin="shared-mmx-test",
    )
    functions = {
        BASE: {
            "name": f"sub_{BASE:08X}",
            "start": f"0x{BASE:08X}",
            "end": BASE + len(code),
            "size": len(code),
        },
    }
    translated = FunctionTranslator(code, functions).translate_function(
        BASE, functions[BASE]
    )

    assert "g_mm0 = (uint64_t)(MEM64(0x15000));" in translated
    assert "MEM64(0x15008) = (uint64_t)(g_mm0);" in translated
    assert "MEM64(0x15010) = (uint64_t)(g_mm0);" in translated
    assert "SSE: movq" not in translated

if __name__ == "__main__":
    test_xmm_register_survives_translated_call()
    test_mmx_movq_preserves_full_64_bit_payload()
    print("ok  shared SIMD state and MMX MOVQ")