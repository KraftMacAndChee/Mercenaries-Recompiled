"""Regression test for x87 state crossing translated function calls.

Run: py -3 tools/recomp/test_shared_x87.py

Xbox x86 functions return floating-point values in ST(0). The translated
callee and caller must therefore use one architectural x87 stack rather than
fresh uninitialized C locals in every generated function.
"""

import math
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.recomp import config  # noqa: E402
from tools.recomp.translator import FunctionTranslator  # noqa: E402


BASE = 0x00012000
# callee: fld dword ptr [0x13000]; ret
# caller: call callee; fstp dword ptr [0x13004]; ret
CODE = bytes.fromhex("d90500300100c3e8f4ffffffd91d04300100c3")
CALLEE_END = BASE + 7


def test_x87_return_uses_shared_architectural_stack():
    config._install(
        [config.Section(".text", BASE, len(CODE), 0, len(CODE), True)],
        entry_point=BASE,
        kernel_thunk_addr=0,
        origin="shared-x87-test",
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

    for source in (callee, caller):
        assert "g_fp_stack" in source
        assert "g_fp_top" in source
        assert "double _fp_stack[8]" not in source
        assert "int _fp_top = 0" not in source
    assert "fp_push(MEMF(0x13000))" in callee
    assert "sub_00012000();" in caller
    assert "MEMF(0x13004) = (float)fp_top(); fp_pop();" in caller


def test_fst_preserves_stack_and_fstp_pops():
    base = 0x00013000
    code = bytes.fromhex(
        "d90500400100"  # fld dword ptr [0x14000]
        "d91504400100"  # fst dword ptr [0x14004]
        "d91d08400100"  # fstp dword ptr [0x14008]
        "c3"
    )
    config._install(
        [config.Section(".text", base, len(code), 0, len(code), True)],
        entry_point=base,
        kernel_thunk_addr=0,
        origin="x87-store-pop-test",
    )
    info = {
        "name": f"sub_{base:08X}",
        "start": f"0x{base:08X}",
        "end": base + len(code),
        "size": len(code),
    }
    source = FunctionTranslator(code, {base: info}).translate_function(
        base, info
    )

    assert "MEMF(0x14004) = (float)fp_top(); /* fst */" in source
    assert "MEMF(0x14008) = (float)fp_top(); fp_pop(); /* fstp */" in source


def test_x87_memory_arithmetic_updates_st0_without_popping():
    base = 0x00015000
    code = bytes.fromhex(
        "d90500600100"  # fld   dword ptr [0x16000]
        "d80504600100"  # fadd  dword ptr [0x16004]
        "d82508600100"  # fsub  dword ptr [0x16008]
        "d82d0c600100"  # fsubr dword ptr [0x1600c]
        "d80d10600100"  # fmul  dword ptr [0x16010]
        "d83514600100"  # fdiv  dword ptr [0x16014]
        "d83d18600100"  # fdivr dword ptr [0x16018]
        "da051c600100"  # fiadd  dword ptr [0x1601c]
        "de0520600100"  # fiadd  word ptr [0x16020]
        "da0d24600100"  # fimul  dword ptr [0x16024]
        "da2528600100"  # fisub  dword ptr [0x16028]
        "da2d2c600100"  # fisubr dword ptr [0x1602c]
        "da3530600100"  # fidiv  dword ptr [0x16030]
        "da3d34600100"  # fidivr dword ptr [0x16034]
        "d91d38600100"  # fstp  dword ptr [0x16038]
        "c3"
    )
    config._install(
        [config.Section(".text", base, len(code), 0, len(code), True)],
        entry_point=base,
        kernel_thunk_addr=0,
        origin="x87-memory-arithmetic-test",
    )
    info = {
        "name": f"sub_{base:08X}",
        "start": f"0x{base:08X}",
        "end": base + len(code),
        "size": len(code),
    }
    source = FunctionTranslator(code, {base: info}).translate_function(
        base, info
    )

    assert "fp_top() += MEMF(0x16004); /* fadd memory */" in source
    assert "fp_top() -= MEMF(0x16008); /* fsub memory */" in source
    assert "fp_top() = MEMF(0x1600C) - fp_top(); /* fsubr memory */" in source
    assert "fp_top() *= MEMF(0x16010); /* fmul memory */" in source
    assert "fp_top() /= MEMF(0x16014); /* fdiv memory */" in source
    assert "fp_top() = MEMF(0x16018) / fp_top(); /* fdivr memory */" in source
    assert "fp_top() += (double)SMEM32(0x1601C); /* fiadd memory */" in source
    assert "fp_top() += (double)SMEM16(0x16020); /* fiadd memory */" in source
    assert "fp_top() *= (double)SMEM32(0x16024); /* fimul memory */" in source
    assert "fp_top() -= (double)SMEM32(0x16028); /* fisub memory */" in source
    assert "fp_top() = (double)SMEM32(0x1602C) - fp_top(); /* fisubr memory */" in source
    assert "fp_top() /= (double)SMEM32(0x16030); /* fidiv memory */" in source
    assert "fp_top() = (double)SMEM32(0x16034) / fp_top(); /* fidivr memory */" in source
def test_x87_register_arithmetic_preserves_or_pops_stack_by_opcode():
    base = 0x00017000
    code = bytes.fromhex(
        "d8c1"  # fadd  st(0), st(1): update top, do not pop
        "dcc1"  # fadd  st(1), st(0): update st(1), do not pop
        "d8e1"  # fsub  st(0), st(1): update top, do not pop
        "d8e9"  # fsubr st(0), st(1): reverse operands, do not pop
        "d8f1"  # fdiv  st(0), st(1): update top, do not pop
        "d8f9"  # fdivr st(0), st(1): reverse operands, do not pop
        "dec1"  # faddp st(1), st(0): update st(1), then pop
        "c3"
    )
    config._install(
        [config.Section(".text", base, len(code), 0, len(code), True)],
        entry_point=base,
        kernel_thunk_addr=0,
        origin="x87-register-arithmetic-test",
    )
    info = {
        "name": f"sub_{base:08X}",
        "start": f"0x{base:08X}",
        "end": base + len(code),
        "size": len(code),
    }
    source = FunctionTranslator(code, {base: info}).translate_function(
        base, info
    )

    assert "fp_top() += fp_st(1); /* fadd */" in source
    assert "fp_st(1) += fp_top(); /* fadd */" in source
    assert "fp_top() -= fp_st(1); /* fsub */" in source
    assert "fp_top() = fp_st(1) - fp_top(); /* fsubr */" in source
    assert "fp_top() /= fp_st(1); /* fdiv */" in source
    assert "fp_top() = fp_st(1) / fp_top(); /* fdivr */" in source
    assert "fp_st(1) += fp_top(); fp_pop(); /* faddp */" in source


def test_fsincos_preserves_x_and_produces_architectural_stack_order():
    base = 0x00018000
    code = bytes.fromhex(
        "d90500900100"  # fld dword ptr [0x19000]
        "d9fb"          # fsincos: ST(0)=cos(x), ST(1)=sin(x)
        "d91d04900100"  # fstp dword ptr [0x19004] (cos)
        "d91d08900100"  # fstp dword ptr [0x19008] (sin)
        "c3"
    )
    config._install(
        [config.Section(".text", base, len(code), 0, len(code), True)],
        entry_point=base,
        kernel_thunk_addr=0,
        origin="x87-fsincos-test",
    )
    info = {
        "name": f"sub_{base:08X}",
        "start": f"0x{base:08X}",
        "end": base + len(code),
        "size": len(code),
    }
    source = FunctionTranslator(code, {base: info}).translate_function(
        base, info
    )

    assert "double _fpu_x = fp_top()" in source
    assert "fp_top() = sin(_fpu_x)" in source
    assert "fp_push(cos(_fpu_x))" in source
    assert "MEMF(0x19004) = (float)fp_top(); fp_pop();" in source
    assert "MEMF(0x19008) = (float)fp_top(); fp_pop();" in source


def test_fyl2x_converts_sample_rate_ratio_to_logarithmic_pitch():
    base = 0x0001A000
    code = bytes.fromhex(
        "d90500b00100"  # fld dword ptr [0x1b000] (pitch scale)
        "d90504b00100"  # fld dword ptr [0x1b004] (source / output rate)
        "d9f1"          # fyl2x: ST(1) *= log2(ST(0)); pop
        "c3"
    )
    config._install(
        [config.Section(".text", base, len(code), 0, len(code), True)],
        entry_point=base,
        kernel_thunk_addr=0,
        origin="x87-fyl2x-test",
    )
    info = {
        "name": f"sub_{base:08X}",
        "start": f"0x{base:08X}",
        "end": base + len(code),
        "size": len(code),
    }
    source = FunctionTranslator(code, {base: info}).translate_function(
        base, info
    )

    assert "fp_st1() *= log2(fp_top()); fp_pop(); /* fyl2x */" in source
    assert round(4096.0 * math.log2(22050.0 / 48000.0)) == -4597


def test_x87_logarithm_companion_instructions_translate_completely():
    base = 0x0001B000
    code = bytes.fromhex(
        "d9ea"  # fldl2e
        "d9f0"  # f2xm1
        "d9fc"  # frndint
        "d9fd"  # fscale
        "c3"
    )
    config._install(
        [config.Section(".text", base, len(code), 0, len(code), True)],
        entry_point=base,
        kernel_thunk_addr=0,
        origin="x87-logarithm-companion-test",
    )
    info = {
        "name": f"sub_{base:08X}",
        "start": f"0x{base:08X}",
        "end": base + len(code),
        "size": len(code),
    }
    source = FunctionTranslator(code, {base: info}).translate_function(
        base, info
    )

    assert "fp_push(1.44269504088896340735992468100189214)" in source
    assert "fp_top() = exp2(fp_top()) - 1.0; /* f2xm1 */" in source
    assert "fp_top() = x87_round_integral(fp_top()); /* frndint */" in source
    assert "fp_top() *= exp2(trunc(fp_st1())); /* fscale */" in source


def test_x87_round_integral_records_precision_in_runtime_template():
    template = os.path.join(
        os.path.dirname(__file__), "..", "..", "templates", "runtime",
        "recomp_types.h",
    )
    text = open(template, encoding="utf-8").read()
    helper = text.split(
        "static inline double x87_round_integral(double value) {", 1
    )[1].split("\n}", 1)[0]
    assert "if (rounded != value)" in helper
    assert "g_x87_status_word |= 0x20u;" in helper


def test_fldpi_pushes_architectural_constant():
    base = 0x0001C000
    code = bytes.fromhex(
        "d9eb"  # fldpi
        "d9ed"  # fldln2
        "d9ec"  # fldlg2
        "c3"
    )
    config._install(
        [config.Section(".text", base, len(code), 0, len(code), True)],
        entry_point=base,
        kernel_thunk_addr=0,
        origin="x87-fldpi-test",
    )
    info = {
        "name": f"sub_{base:08X}",
        "start": f"0x{base:08X}",
        "end": base + len(code),
        "size": len(code),
    }
    source = FunctionTranslator(code, {base: info}).translate_function(
        base, info
    )

    assert "fp_push(3.141592653589793238462643383279502884)" in source
    assert "fp_push(0.693147180559945309417232121458176568)" in source
    assert "fp_push(0.301029995663981195213738894724493027)" in source


if __name__ == "__main__":
    test_x87_return_uses_shared_architectural_stack()
    test_fst_preserves_stack_and_fstp_pops()
    test_x87_memory_arithmetic_updates_st0_without_popping()
    test_x87_register_arithmetic_preserves_or_pops_stack_by_opcode()
    test_fsincos_preserves_x_and_produces_architectural_stack_order()
    test_fyl2x_converts_sample_rate_ratio_to_logarithmic_pitch()
    test_x87_logarithm_companion_instructions_translate_completely()
    test_fldpi_pushes_architectural_constant()
    print("ok  shared_x87_state_store_and_memory_arithmetic")