"""Self-check for immediate-operand address references. Synthetic bytes only.

String literals are referenced as immediates (`push offset str`), not as memory
operands, so a database that only records CS_OP_MEM sees almost none of them.
These cases pin that behaviour down.

    py -3 tools/disasm/test_imm_refs.py
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))))

from tools.disasm.engine import DisasmEngine

BASE = 0x00010000
SIZE = 0x00100000
IN_RANGE = 0x00012345          # inside the image
OUT_OF_RANGE = 0x7F000000      # outside the image


class FakeImage:
    """Minimal stand-in for BinaryImage: classification only reads these."""
    base_address = BASE
    image_size = SIZE


def decode(engine, code, va=BASE):
    insns = list(engine._cs.disasm(code, va))
    assert len(insns) == 1, f"expected 1 instruction, decoded {len(insns)}"
    return engine._classify_instruction(insns[0])


def imm32(v):
    return bytes([v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF, (v >> 24) & 0xFF])


def main():
    engine = DisasmEngine(FakeImage())
    checks = 0

    # push offset IN_RANGE  ->  imm_ref, the canonical string reference
    i = decode(engine, b"\x68" + imm32(IN_RANGE))
    assert i.imm_ref == IN_RANGE, f"push offset: imm_ref={i.imm_ref!r}"
    assert i.memory_ref is None, "push offset must not set memory_ref"
    checks += 1

    # mov eax, offset IN_RANGE  ->  imm_ref
    i = decode(engine, b"\xB8" + imm32(IN_RANGE))
    assert i.imm_ref == IN_RANGE, f"mov reg,offset: imm_ref={i.imm_ref!r}"
    checks += 1

    # mov eax, [IN_RANGE]  ->  memory_ref, NOT imm_ref
    i = decode(engine, b"\xA1" + imm32(IN_RANGE))
    assert i.memory_ref == IN_RANGE, f"mov reg,[addr]: memory_ref={i.memory_ref!r}"
    assert i.imm_ref is None, "memory operand must not set imm_ref"
    checks += 1

    # An immediate outside the image is a constant, not an address.
    i = decode(engine, b"\x68" + imm32(OUT_OF_RANGE))
    assert i.imm_ref is None, f"out-of-range imm must be ignored: {i.imm_ref!r}"
    checks += 1

    # push 5 -- small constant, nowhere near the image
    i = decode(engine, b"\x6A\x05")
    assert i.imm_ref is None, f"push imm8 must be ignored: {i.imm_ref!r}"
    checks += 1

    # call rel32: the immediate is the call target, not a data reference
    i = decode(engine, b"\xE8" + imm32(0x100))
    assert i.is_call, "E8 should classify as a call"
    assert i.imm_ref is None, "call target must not be recorded as a data ref"
    checks += 1

    # jmp rel32: likewise
    i = decode(engine, b"\xE9" + imm32(0x100))
    assert i.is_jump, "E9 should classify as a jump"
    assert i.imm_ref is None, "jump target must not be recorded as a data ref"
    checks += 1

    print(f"OK: {checks} immediate-reference cases passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
