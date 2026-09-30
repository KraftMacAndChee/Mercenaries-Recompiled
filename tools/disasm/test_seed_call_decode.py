"""Regression check for exact decoding of callees reached from seeds.

    py -3 tools/disasm/test_seed_call_decode.py
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))))

from tools.disasm.engine import DisasmEngine, Instruction


class FakeImage:
    base_address = 0x1000
    image_size = 0x20

    def __init__(self):
        self.data = bytearray(0x20)
        self.data[0:6] = bytes.fromhex("E80B000000C3")
        self.data[0x10:0x16] = bytes.fromhex("B82A000000C3")

    def read_bytes_at_va(self, address, size):
        offset = address - self.base_address
        if offset < 0 or offset >= len(self.data):
            return b""
        return bytes(self.data[offset:offset + size])


class OverlappingSeedImage:
    base_address = 0x1000
    image_size = 0x10

    def __init__(self):
        self.data = bytearray(0x10)
        # 1000: nop
        # 1001: mov eax, 0x0000C390 (crosses reviewed seed 1002)
        # 1002: nop; 1003: ret
        self.data[0:7] = bytes.fromhex("90 B8 90 C3 00 00 C3")

    def read_bytes_at_va(self, address, size):
        offset = address - self.base_address
        if offset < 0 or offset >= len(self.data):
            return b""
        return bytes(self.data[offset:offset + size])


def main():
    engine = DisasmEngine(FakeImage())
    engine.instructions[0x100F] = Instruction(
        0x100F, 2, "add", "byte ptr [esi], al", "0000")

    engine.decode_seed_functions([0x1000], [(0x1000, 0x1020)])

    assert engine.instructions[0x1000].call_target == 0x1010
    assert engine.instructions[0x1010].mnemonic == "mov"
    assert 0x100F not in engine.instructions

    # Seeds are reviewed hard boundaries. Recursive flow from an earlier seed
    # must not install a misaligned instruction across a later one and erase
    # it merely because the worklist happens to process the earlier seed last.
    engine = DisasmEngine(OverlappingSeedImage())
    engine.decode_seed_functions(
        [0x1000, 0x1002], [(0x1000, 0x1010)], [0x1000, 0x1002]
    )
    assert engine.instructions[0x1002].mnemonic == "nop"
    assert engine.instructions[0x1003].is_ret
    assert 0x1001 not in engine.instructions

    print("OK: exact seed decoding follows callees and protects reviewed boundaries")
    return 0


if __name__ == "__main__":
    sys.exit(main())
