"""Regression coverage for MMIO-safe REP MOVS lowering."""

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.recomp import config  # noqa: E402
from tools.recomp.translator import FunctionTranslator  # noqa: E402


BASE = 0x00015000


def test_rep_movsd_uses_guest_mmio_safe_copy():
    code = bytes.fromhex("f3a5c3")  # rep movsd; ret
    config._install(
        [config.Section(".text", BASE, len(code), 0, len(code), True)],
        entry_point=BASE,
        kernel_thunk_addr=0,
        origin="rep-movs-mmio-test",
    )
    fn = {
        "name": f"sub_{BASE:08X}", "start": f"0x{BASE:08X}",
        "end": BASE + len(code), "size": len(code),
    }
    source = FunctionTranslator(code, {BASE: fn}).translate_function(BASE, fn)
    assert "XBOX_REP_MOVS(edi, esi, ecx, 4u);" in source
    assert "memcpy((void*)XBOX_PTR" not in source


if __name__ == "__main__":
    test_rep_movsd_uses_guest_mmio_safe_copy()
    print("ok  rep_movs_mmio")