#!/usr/bin/env python3
"""Regression coverage for unsigned branches after logical instructions."""

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
        origin="logical-unsigned-condition-test",
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


def test_and_jbe_branches_when_result_is_zero():
    # and ecx,edi; jbe +1; ret; ret
    source = _translate(bytes.fromhex("21f97601c3c3"))
    assert "if ((ecx == 0)) goto" in source
    assert "if (0) goto" not in source


def test_and_ja_branches_when_result_is_nonzero():
    # and ecx,edi; ja +1; ret; ret
    source = _translate(bytes.fromhex("21f97701c3c3"))
    assert "if ((ecx != 0)) goto" in source
    assert "if (1) goto" not in source


if __name__ == "__main__":
    test_and_jbe_branches_when_result_is_zero()
    test_and_ja_branches_when_result_is_nonzero()
    print("ok logical_unsigned_conditions")