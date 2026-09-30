"""
Regression test for unresolved direct-call stubs.

Run: py -3 tools/recomp/test_unresolved_stubs.py

Translated direct calls push a synthetic guest return address before calling
their C implementation. An unresolved target must consume that address even
though its behavior is not yet implemented, or every hit corrupts the guest
stack by four bytes.
"""

import os
import sys
import tempfile
from types import SimpleNamespace

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.recomp.translator import BatchTranslator  # noqa: E402


class _FakeTranslator:
    def __init__(self):
        self.lifter = SimpleNamespace(
            referenced_calls={0x12345678: "sub_12345678"}
        )

    def translate_function(self, addr, func_info):
        return "void sub_00001000(void) { esp += 4; }\n"


def test_unresolved_stub_consumes_guest_return_address():
    batch = BatchTranslator.__new__(BatchTranslator)
    batch.translator = _FakeTranslator()

    functions = [(0x1000, {"name": "sub_00001000"})]
    with tempfile.TemporaryDirectory() as output_dir:
        stats = batch.translate_batch_split(
            functions, output_dir, chunk_size=1000, verbose=False
        )
        stub_path = os.path.join(output_dir, "recomp_stubs_unresolved.c")
        with open(stub_path, "r", encoding="utf-8") as f:
            source = f.read()

    assert stats["unresolved_stubs"] == 1
    assert (
        "void sub_12345678(void) { esp += 4; "
        "/* 0x12345678: not detected; minimal guest ret */ }"
    ) in source
    print("ok  unresolved_stub_consumes_guest_return_address")


if __name__ == "__main__":
    test_unresolved_stub_consumes_guest_return_address()
    print("\nall passed")
