"""Regression coverage for 8/16-bit CMP condition semantics."""

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.recomp import config  # noqa: E402
from tools.recomp.translator import FunctionTranslator  # noqa: E402

BASE = 0x00016000


def _translate(code: bytes) -> str:
    config._install(
        [config.Section(".text", BASE, len(code), 0, len(code), True)],
        entry_point=BASE,
        kernel_thunk_addr=0,
        origin="narrow-cmp-test",
    )
    functions = {BASE: {"name": f"sub_{BASE:08X}", "start": f"0x{BASE:08X}",
                        "end": BASE + len(code), "size": len(code)}}
    return FunctionTranslator(code, functions).translate_function(BASE, functions[BASE])


def test_cmp_word_memory_jl_sign_extends_operand():
    source = _translate(bytes.fromhex("66833f007c01c3c3"))
    assert "((int16_t)(MEM16(edi)) < (int16_t)(0))" in source


def test_cmp_byte_register_jge_sign_extends_operands():
    source = _translate(bytes.fromhex("3cff7d01c3c3"))
    assert "((int8_t)(LO8(eax)) >= (int8_t)(0xFF))" in source


def test_cmp_word_unsigned_truncates_immediate():
    source = _translate(bytes.fromhex("663dffff7201c3c3"))
    assert "((uint16_t)(LO16(eax)) < (uint16_t)(0xFFFF))" in source


def test_test_byte_register_jge_uses_byte_sign():
    source = _translate(bytes.fromhex("84c07d01c3c3"))
    assert "(int8_t)((LO8(eax) & LO8(eax))) >= (int8_t)(0)" in source


def test_test_byte_register_jle_uses_byte_sign_and_zero():
    source = _translate(bytes.fromhex("84c07e01c3c3"))
    assert "(int8_t)((LO8(eax) & LO8(eax))) <= (int8_t)(0)" in source

if __name__ == "__main__":
    test_cmp_word_memory_jl_sign_extends_operand()
    test_cmp_byte_register_jge_sign_extends_operands()
    test_cmp_word_unsigned_truncates_immediate()
    test_test_byte_register_jge_uses_byte_sign()
    test_test_byte_register_jle_uses_byte_sign_and_zero()
    print("ok  narrow_cmp")