#!/usr/bin/env python3
"""Locate and disassemble x64 PE runtime functions containing supplied RVAs."""

from __future__ import annotations

import argparse
import struct
from pathlib import Path

from capstone import CS_ARCH_X86, CS_MODE_64, CS_OP_MEM, Cs
from capstone.x86_const import X86_REG_RIP


def u16(data: bytes, offset: int) -> int:
    return struct.unpack_from("<H", data, offset)[0]


def u32(data: bytes, offset: int) -> int:
    return struct.unpack_from("<I", data, offset)[0]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("image", type=Path)
    parser.add_argument("rvas", nargs="+", type=lambda value: int(value, 0))
    parser.add_argument("--instructions", type=int, default=48)
    args = parser.parse_args()

    data = args.image.read_bytes()
    pe = u32(data, 0x3C)
    if data[pe : pe + 4] != b"PE\0\0":
        raise SystemExit("not a PE image")
    section_count = u16(data, pe + 6)
    optional_size = u16(data, pe + 20)
    optional = pe + 24
    if u16(data, optional) != 0x20B:
        raise SystemExit("only PE32+ images are supported")
    image_base = struct.unpack_from("<Q", data, optional + 24)[0]
    directories = optional + 112
    import_rva = u32(data, directories + 1 * 8)
    import_size = u32(data, directories + 1 * 8 + 4)
    exception_rva = u32(data, directories + 3 * 8)
    exception_size = u32(data, directories + 3 * 8 + 4)
    section_table = optional + optional_size
    sections: list[tuple[str, int, int, int, int]] = []
    for index in range(section_count):
        entry = section_table + index * 40
        name = data[entry : entry + 8].split(b"\0", 1)[0].decode("ascii")
        virtual_size = u32(data, entry + 8)
        virtual_address = u32(data, entry + 12)
        raw_size = u32(data, entry + 16)
        raw_offset = u32(data, entry + 20)
        sections.append((name, virtual_address, virtual_size, raw_offset, raw_size))

    def rva_offset(rva: int) -> int:
        for _, va, virtual_size, raw, raw_size in sections:
            if va <= rva < va + max(virtual_size, raw_size):
                delta = rva - va
                if delta >= raw_size:
                    raise ValueError(f"RVA 0x{rva:X} has no file-backed data")
                return raw + delta
        raise ValueError(f"RVA 0x{rva:X} is outside all sections")

    def c_string(offset: int) -> str:
        end = data.find(b"\0", offset)
        if end < 0:
            end = len(data)
        return data[offset:end].decode("ascii", errors="replace")

    imports: dict[int, str] = {}
    if import_rva and import_size:
        descriptor = rva_offset(import_rva)
        while descriptor + 20 <= len(data):
            original_thunk, _, _, name_rva, first_thunk = struct.unpack_from(
                "<IIIII", data, descriptor
            )
            if not any((original_thunk, name_rva, first_thunk)):
                break
            dll = c_string(rva_offset(name_rva))
            lookup_rva = original_thunk or first_thunk
            index = 0
            while True:
                thunk = struct.unpack_from(
                    "<Q", data, rva_offset(lookup_rva + index * 8)
                )[0]
                if thunk == 0:
                    break
                if thunk & (1 << 63):
                    symbol = f"ordinal#{thunk & 0xFFFF}"
                else:
                    symbol = c_string(rva_offset(thunk) + 2)
                imports[first_thunk + index * 8] = f"{dll}!{symbol}"
                index += 1
            descriptor += 20

    pdata = rva_offset(exception_rva)
    runtime_functions = [
        struct.unpack_from("<III", data, pdata + offset)
        for offset in range(0, exception_size - 11, 12)
    ]
    decoder = Cs(CS_ARCH_X86, CS_MODE_64)
    decoder.detail = True
    for target in args.rvas:
        containing = next(
            (entry for entry in runtime_functions if entry[0] <= target < entry[1]),
            None,
        )
        print(f"target RVA=0x{target:X} VA=0x{image_base + target:X}")
        if containing is None:
            print("  no containing runtime function")
            continue
        begin, end, unwind = containing
        print(
            f"  function RVA=0x{begin:X}..0x{end:X} size=0x{end-begin:X} "
            f"unwind=0x{unwind:X} offset=+0x{target-begin:X}"
        )
        code = data[rva_offset(begin) : rva_offset(begin) + end - begin]
        instructions = list(decoder.disasm(code, image_base + begin))
        target_va = image_base + target
        nearest = min(
            range(len(instructions)),
            key=lambda index: abs(instructions[index].address - target_va),
        )
        first = max(0, nearest - 12)
        last = min(len(instructions), nearest + args.instructions)
        for instruction in instructions[first:last]:
            marker = ">" if instruction.address <= target_va < (
                instruction.address + instruction.size
            ) else " "
            annotation = ""
            for operand in instruction.operands:
                if operand.type == CS_OP_MEM and operand.mem.base == X86_REG_RIP:
                    referenced_va = (
                        instruction.address + instruction.size + operand.mem.disp
                    )
                    import_name = imports.get(referenced_va - image_base)
                    if import_name:
                        annotation = f" ; {import_name}"
                        break
            print(
                f" {marker} {instruction.address:016X}  "
                f"{instruction.mnemonic:<8} {instruction.op_str}{annotation}"
            )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
