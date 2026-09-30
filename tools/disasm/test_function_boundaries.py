"""Self-checks for control-flow-aware function boundary detection.

    py -3 tools/disasm/test_function_boundaries.py
"""
import os
import sys
from types import SimpleNamespace

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))))

from tools.disasm.engine import Instruction
from tools.disasm.functions import Function, FunctionDetector


def insn(address, size=2, *, jump=False, conditional=False, target=None,
         ret=False):
    return Instruction(
        address=address,
        size=size,
        mnemonic="ret" if ret else ("jne" if conditional else
                                     ("jmp" if jump else "nop")),
        op_str="",
        bytes_hex="",
        is_jump=jump or conditional,
        is_cond_jump=conditional,
        is_ret=ret,
        jump_target=target,
    )


class FakeEngine:
    def __init__(self, instructions):
        self.instructions = {i.address: i for i in instructions}

    def get_instruction(self, address):
        return self.instructions.get(address)

    def get_instructions_in_range(self, start, end):
        return [self.instructions[a] for a in sorted(self.instructions)
                if start <= a < end]


class FakeImage:
    def __init__(self, section_data=b""):
        self.section_data = section_data

    def get_section_at_va(self, address):
        return SimpleNamespace(executable=0x1000 <= address < 0x4000)

    def get_section_data(self, section):
        return self.section_data


def main():
    # This mirrors the title-startup shape that previously truncated
    # Mercenaries at the JMP: the conditional target and JMP target both lie
    # beyond the JMP, and eventually reconverge at the RET.
    engine = FakeEngine([
        insn(0x1000),
        insn(0x1002, conditional=True, target=0x1008),
        insn(0x1004),
        insn(0x1006, jump=True, target=0x100A),
        insn(0x1008),
        insn(0x100A),
        insn(0x100C, size=1, ret=True),
    ])
    detector = FunctionDetector(engine, None, None, None)
    end = detector._find_function_end(0x1000, 0x1100, 0x1200)
    assert end == 0x100D, f"expected 0x100D, got 0x{end:X}"

    # A jump outside the current candidate's bounds is a tail call and must
    # not absorb the following function.
    engine = FakeEngine([
        insn(0x2000),
        insn(0x2002, jump=True, target=0x2100),
        insn(0x2004),
    ])
    detector = FunctionDetector(engine, None, None, None)
    end = detector._find_function_end(0x2000, 0x2100, 0x2200)
    assert end == 0x2004, f"expected 0x2004, got 0x{end:X}"

    # A direct unconditional jump outside a provisional function is a tail
    # call. Its destination must become a function seed; internal and
    # conditional branches must not be split.
    engine = FakeEngine([
        insn(0x3000),
        insn(0x3002, jump=True, target=0x3100),
        insn(0x3010, conditional=True, target=0x3200),
        insn(0x3100, size=1, ret=True),
        insn(0x3200, size=1, ret=True),
    ])
    detector = FunctionDetector(engine, FakeImage(), None, None)
    detector.functions[0x3000] = Function(
        start=0x3000, end=0x3012, name="sub_00003000", section=".text",
        confidence=0.9, detection_method="call_target", num_instructions=3,
        has_prologue=False)
    assert detector._pass_tail_jump_targets() == 1
    assert detector._candidates[0x3100][1] == "tail_jump_target"
    assert 0x3200 not in detector._candidates

    # Frame-pointer-omitted MSVC C++ functions commonly register an SEH
    # handler instead of beginning with push ebp/mov ebp,esp. The exact FS
    # chain load/store sequence is entry evidence; a near match is not.
    valid_seh = bytes.fromhex(
        "90 CC 6A FF 68 78 56 34 12 64 A1 00 00 00 00 50 "
        "64 89 25 00 00 00 00 90")
    invalid_seh = bytes.fromhex(
        "6A FF 68 78 56 34 12 64 A1 00 00 00 01 50 "
        "64 89 25 00 00 00 00")
    image = FakeImage(valid_seh + invalid_seh)
    section = SimpleNamespace(virtual_addr=0x1000)
    engine = FakeEngine([insn(0x1002), insn(0x1018)])
    detector = FunctionDetector(engine, image, None, None)
    detector._pass_prologues(section)
    assert detector._candidates[0x1002][1] == "seh_prologue"
    assert 0x1018 not in detector._candidates

    print("OK: 4 function-boundary cases passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
