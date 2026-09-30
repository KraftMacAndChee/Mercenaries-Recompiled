"""Regression coverage for x86 floating-point status/LAHF parity idioms."""

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.recomp import config  # noqa: E402
from tools.recomp.translator import FunctionTranslator  # noqa: E402


BASE = 0x00013000


def _translate(code: bytes) -> str:
    config._install(
        [config.Section(".text", BASE, len(code), 0, len(code), True)],
        entry_point=BASE,
        kernel_thunk_addr=0,
        origin="float-status-flags-test",
    )
    info = {
        "name": f"sub_{BASE:08X}",
        "start": f"0x{BASE:08X}",
        "end": BASE + len(code),
        "size": len(code),
    }
    return FunctionTranslator(code, {BASE: info}).translate_function(BASE, info)


def test_ucomiss_lahf_test_parity_materializes_ah():
    source = _translate(bytes.fromhex(
        "f30f100500400100"  # movss xmm0, dword ptr [0x14000]
        "0f2e0504400100"    # ucomiss xmm0, dword ptr [0x14004]
        "9f"                # lahf
        "f6c444"            # test ah, 0x44
        "7a01"              # jp +1
        "c3c3"
    ))

    assert "SET_HI8(eax," in source
    assert "isnan((double)(xmm0))" in source
    assert "EVEN_PARITY8((uint8_t)(HI8(eax) & 0x44))" in source
    assert "jp after test - parity" not in source


def test_ucomiss_lahf_snapshots_before_operand_reload():
    # Retail RsLight::RecalculateLight compares hibernation distance against
    # zero, reloads xmm0 with the 100-unit default draw distance, then uses
    # LAHF. MOVSS preserves EFLAGS, so LAHF must see the pre-reload comparison.
    source = _translate(bytes.fromhex(
        "f30f100500400100"  # movss xmm0, dword ptr [0x14000]
        "0f57c9"            # xorps xmm1, xmm1
        "0f2ec1"            # ucomiss xmm0, xmm1
        "f30f100508400100"  # movss xmm0, dword ptr [0x14008]
        "9f"                # lahf
        "f6c444"            # test ah, 0x44
        "7b01"              # jnp +1
        "c3c3"
    ))

    snapshot = "_flags = (int)(((isnan((double)(xmm0))"
    reload = "recomp_xmm_loadss(xmm0v, 0x14008);"
    assert snapshot in source
    assert source.index(snapshot) < source.index(reload)
    assert "SET_HI8(eax, (uint32_t)_flags);" in source
    assert "lahf from snapshotted ucomiss" in source


def test_fucompp_stores_status_and_pops_twice():
    source = _translate(bytes.fromhex(
        "dd0500400100"  # fld qword ptr [0x14000]
        "dd0508400100"  # fld qword ptr [0x14008]
        "dae9"          # fucompp
        "dfe0"          # fnstsw ax
        "f6c444"        # test ah, 0x44
        "7a01"          # jp +1
        "c3c3"
    ))

    assert "g_x87_status_word" in source
    assert "SET_LO16(eax, g_x87_status_word)" in source
    assert "fp_pop(); fp_pop();" in source
    assert "EVEN_PARITY8((uint8_t)(HI8(eax) & 0x44))" in source
    assert "jp after test - parity" not in source


def test_x87_memory_compare_and_control_state_are_architectural():
    compare = _translate(bytes.fromhex(
        "d90500400100"  # fld dword ptr [0x14000]
        "d81d04400100"  # fcomp dword ptr [0x14004]
        "c3"
    ))
    assert "double _fpu_b = MEMF(0x14004)" in compare
    assert "fp_pop();" in compare

    control = _translate(bytes.fromhex(
        "d93d00400100"  # fnstcw word ptr [0x14000]
        "d92d00400100"  # fldcw word ptr [0x14000]
        "dbe2"          # fnclex
        "c3"
    ))
    assert "MEM16(0x14000) = g_x87_control_word" in control
    assert "g_x87_control_word = (uint16_t)MEM16(0x14000)" in control
    assert "g_x87_status_word &= 0x7F00u" in control
def test_fnstsw_sahf_parity_reads_ah_and_fprem_completes():
    source = _translate(bytes.fromhex(
        "d9f8"  # fprem
        "dfe0"  # fnstsw ax
        "9e"    # sahf
        "7a01"  # jp +1
        "c3c3"
    ))
    assert "fmod(fp_top(), fp_st1())" in source
    assert "g_x87_status_word &= (uint16_t)~0x0400u" in source
    assert "if (((HI8(eax) & 0x04u) != 0))" in source
    assert "_fpu_cmp" not in source
def test_integer_test_parity_is_not_constant():
    source = _translate(bytes.fromhex("84c07a01c3c3"))  # test al, al; jp +1
    assert "EVEN_PARITY8((uint8_t)(LO8(eax) & LO8(eax)))" in source
    assert "jp after test - parity" not in source


def test_x87_register_stack_operands_use_requested_index():
    source = _translate(bytes.fromhex(
        "d9c2"  # fld st(2)
        "dddb"  # fstp st(3)
        "d9ca"  # fxch st(2)
        "d8da"  # fcomp st(2)
        "c3"
    ))
    assert "double _fpu_t = fp_st(2); fp_push(_fpu_t);" in source
    assert "fp_st(3) = fp_top(); fp_pop();" in source
    assert "fp_top() = fp_st(2); fp_st(2) = _t;" in source
    assert "double _fpu_b = fp_st(2);" in source
    assert "#define fp_st(i)" in source
    assert "#undef fp_st" in source


def test_x87_transcendentals_preserve_stack_contracts():
    source = _translate(bytes.fromhex(
        "d9fe"  # fsin
        "d9ff"  # fcos
        "d9f2"  # fptan
        "ddd8"  # fstp st(0), discards FPTAN's pushed 1.0
        "d9f3"  # fpatan
        "c3"
    ))
    assert "fp_top() = sin(fp_top())" in source
    assert "fp_top() = cos(fp_top())" in source
    assert "fp_top() = tan(_fpu_x)" in source
    assert "fp_push(1.0)" in source
    assert "fp_st1() = atan2(fp_st1(), fp_top()); fp_pop()" in source


def test_fucomi_lahf_materializes_x87_eflags_in_ah():
    source = _translate(bytes.fromhex(
        "dd0500400100"  # fld qword ptr [0x14000]
        "d9c0"          # fld st(0)
        "d9c9"          # fxch st(1)
        "dbe9"          # fucomi st, st(1)
        "ddd8"          # fstp st(0)
        "9f"            # lahf
        "f6c444"        # test ah, 0x44
        "7b01"          # jnp +1
        "c3c3"
    ))
    assert "SET_HI8(eax, ((_fpu_cmp == 2) ? 0x45u" in source
    assert "lahf from fucomi" in source
    assert "EVEN_PARITY8((uint8_t)(HI8(eax) & 0x44))" in source

def test_x87_qword_integer_load_and_store_use_64_bits():
    source = _translate(bytes.fromhex(
        "df2d00400100"  # fild qword ptr [0x14000]
        "df3d08400100"  # fistp qword ptr [0x14008]
        "c3"
    ))
    assert "fp_push((double)SMEM64(0x14000))" in source
    assert "MEM64(0x14008) = X87_FIST64(fp_top()); fp_pop();" in source

if __name__ == "__main__":
    test_ucomiss_lahf_test_parity_materializes_ah()
    test_ucomiss_lahf_snapshots_before_operand_reload()
    test_fucompp_stores_status_and_pops_twice()
    test_x87_memory_compare_and_control_state_are_architectural()
    test_fnstsw_sahf_parity_reads_ah_and_fprem_completes()
    test_integer_test_parity_is_not_constant()
    test_x87_register_stack_operands_use_requested_index()
    test_fucomi_lahf_materializes_x87_eflags_in_ah()
    test_x87_qword_integer_load_and_store_use_64_bits()
    print("ok  float_status_flags")
