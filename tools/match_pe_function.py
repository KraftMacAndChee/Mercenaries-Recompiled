#!/usr/bin/env python3
"""Find semantically similar x64 PE runtime functions across two builds."""

from __future__ import annotations

import argparse
import difflib
import struct
from pathlib import Path

from capstone import CS_ARCH_X86, CS_MODE_64, CS_OP_IMM, CS_OP_MEM, CS_OP_REG, Cs
from capstone.x86_const import X86_REG_RIP


def u16(data: bytes, offset: int) -> int:
    return struct.unpack_from("<H", data, offset)[0]


def u32(data: bytes, offset: int) -> int:
    return struct.unpack_from("<I", data, offset)[0]


class Image:
    def __init__(self, path: Path) -> None:
        self.path = path
        self.data = path.read_bytes()
        pe = u32(self.data, 0x3C)
        if self.data[pe : pe + 4] != b"PE\0\0":
            raise ValueError(f"{path} is not a PE image")
        section_count = u16(self.data, pe + 6)
        optional_size = u16(self.data, pe + 20)
        optional = pe + 24
        if u16(self.data, optional) != 0x20B:
            raise ValueError("only PE32+ images are supported")
        self.image_base = struct.unpack_from("<Q", self.data, optional + 24)[0]
        directories = optional + 112
        exception_rva = u32(self.data, directories + 3 * 8)
        exception_size = u32(self.data, directories + 3 * 8 + 4)
        section_table = optional + optional_size
        self.sections: list[tuple[int, int, int, int]] = []
        for index in range(section_count):
            entry = section_table + index * 40
            self.sections.append(
                (
                    u32(self.data, entry + 12),
                    u32(self.data, entry + 8),
                    u32(self.data, entry + 20),
                    u32(self.data, entry + 16),
                )
            )
        pdata = self.offset(exception_rva)
        self.functions = [
            struct.unpack_from("<III", self.data, pdata + offset)
            for offset in range(0, exception_size - 11, 12)
        ]

    def offset(self, rva: int) -> int:
        for va, virtual_size, raw, raw_size in self.sections:
            if va <= rva < va + max(virtual_size, raw_size):
                delta = rva - va
                if delta >= raw_size:
                    raise ValueError(f"RVA 0x{rva:X} has no file data")
                return raw + delta
        raise ValueError(f"RVA 0x{rva:X} is outside the image")

    def containing(self, rva: int) -> tuple[int, int, int]:
        return next(entry for entry in self.functions if entry[0] <= rva < entry[1])

    def code(self, entry: tuple[int, int, int]) -> bytes:
        begin, end, _ = entry
        return self.data[self.offset(begin) : self.offset(begin) + end - begin]


def fingerprint(image: Image, entry: tuple[int, int, int], decoder: Cs) -> list[str]:
    begin, _, _ = entry
    result: list[str] = []
    for instruction in decoder.disasm(image.code(entry), image.image_base + begin):
        operands: list[str] = []
        for operand in instruction.operands:
            if operand.type == CS_OP_REG:
                operands.append(decoder.reg_name(operand.reg))
            elif operand.type == CS_OP_IMM:
                if instruction.group(1) or instruction.group(2):
                    operands.append("branch")
                else:
                    value = operand.imm & 0xFFFFFFFFFFFFFFFF
                    operands.append(hex(value) if value <= 0xFFFFFF else "imm")
            elif operand.type == CS_OP_MEM:
                base = decoder.reg_name(operand.mem.base) if operand.mem.base else ""
                index = decoder.reg_name(operand.mem.index) if operand.mem.index else ""
                displacement = "rip" if operand.mem.base == X86_REG_RIP else hex(
                    operand.mem.disp & 0xFFFFFFFFFFFFFFFF
                )
                operands.append(
                    f"mem:{base}:{index}:{operand.mem.scale}:{displacement}"
                )
            else:
                operands.append(str(operand.type))
        result.append(f"{instruction.mnemonic} {'|'.join(operands)}")
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path)
    parser.add_argument("target", type=Path)
    parser.add_argument("rva", type=lambda value: int(value, 0))
    parser.add_argument("--count", type=int, default=12)
    args = parser.parse_args()

    source = Image(args.source)
    target = Image(args.target)
    source_entry = source.containing(args.rva)
    source_size = source_entry[1] - source_entry[0]
    decoder = Cs(CS_ARCH_X86, CS_MODE_64)
    decoder.detail = True
    source_fp = fingerprint(source, source_entry, decoder)
    candidates: list[tuple[float, tuple[int, int, int], int]] = []
    for entry in target.functions:
        size = entry[1] - entry[0]
        if size < source_size * 0.65 or size > source_size * 1.35:
            continue
        candidate_fp = fingerprint(target, entry, decoder)
        if not candidate_fp:
            continue
        ratio = difflib.SequenceMatcher(None, source_fp, candidate_fp).ratio()
        candidates.append((ratio, entry, len(candidate_fp)))
    candidates.sort(reverse=True, key=lambda item: item[0])
    print(
        f"source RVA=0x{source_entry[0]:X}..0x{source_entry[1]:X} "
        f"size=0x{source_size:X} instructions={len(source_fp)}"
    )
    for ratio, entry, instruction_count in candidates[: args.count]:
        print(
            f"score={ratio:.5f} target RVA=0x{entry[0]:X}..0x{entry[1]:X} "
            f"size=0x{entry[1]-entry[0]:X} instructions={instruction_count}"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
