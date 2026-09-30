"""Regression coverage for natural fallthrough across split functions."""

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.recomp import config  # noqa: E402
from tools.recomp.translator import FunctionTranslator  # noqa: E402


BASE = 0x00014000


def _translate(code: bytes, first_end: int) -> str:
    config._install(
        [config.Section(".text", BASE, len(code), 0, len(code), True)],
        entry_point=BASE,
        kernel_thunk_addr=0,
        origin="split-fallthrough-test",
    )
    functions = {
        BASE: {
            "name": f"sub_{BASE:08X}",
            "start": f"0x{BASE:08X}",
            "end": first_end,
            "size": first_end - BASE,
        },
        first_end: {
            "name": f"sub_{first_end:08X}",
            "start": f"0x{first_end:08X}",
            "end": BASE + len(code),
            "size": BASE + len(code) - first_end,
        },
    }
    return FunctionTranslator(code, functions).translate_function(
        BASE, functions[BASE]
    )


def test_normal_instruction_falls_through_to_adjacent_function():
    # mov eax, edx | ret
    source = _translate(bytes.fromhex("8bc2c3"), BASE + 2)
    assert f"sub_{BASE + 2:08X}(); return; /* fallthrough" in source


def test_conditional_jump_keeps_false_fallthrough_edge():
    # test dl, 0x10; je +1 | ret | ret
    source = _translate(bytes.fromhex("f6c2107401c3c3"), BASE + 5)
    assert f"sub_{BASE + 5:08X}(); return; /* fallthrough" in source


if __name__ == "__main__":
    test_normal_instruction_falls_through_to_adjacent_function()
    test_conditional_jump_keeps_false_fallthrough_edge()
    print("ok  split_fallthrough")
